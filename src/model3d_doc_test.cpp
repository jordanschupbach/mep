// Windowless coverage for model3d_doc.h's raylib-*independent* half: Scene
// bookkeeping and the glTF JSON exporter. Deliberately does NOT call
// LoadModel3DFile/AddPrimitiveToScene -- both go through raylib's
// LoadModel/GenMesh*/UploadMesh, which need a live GL context (an
// initialized raylib window) to actually run, the one thing this binary
// (like mep-html-doc-test) is meant to avoid needing. Meshes here are
// built by hand instead, matching the same MeshData shape those functions
// would produce.

#include "model3d_doc.h"

#include "json.h"
#include "raylib.h"

#include <unistd.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <sstream>

namespace {
void Check(bool condition, const char *expression, int line) {
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

// A single unit triangle in the XY plane, its own flat normal, and one UV
// per vertex -- the minimal MeshData a real primitive/import would produce.
MeshData MakeTriangle() {
    MeshData md;
    md.name = "triangle";
    md.positions = {0.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 0.0f, 1.0f, 0.0f};
    md.normals = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    md.texcoords = {0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f};
    md.indices = {0, 1, 2};
    return md;
}
}  // namespace

int main() {
    // --- Scene bookkeeping ---
    Scene scene;
    int mesh_index = static_cast<int>(scene.meshes.size());
    scene.meshes.push_back(MakeTriangle());

    Object3D a;
    a.name = "A";
    a.mesh_index = mesh_index;
    a.kind = PrimitiveKind::Cube;
    a.color = RgbaColorF{1.0f, 0.0f, 0.0f, 1.0f};
    int id_a = scene.AddObject(a);

    Object3D b;
    b.name = "B";
    b.mesh_index = mesh_index;
    b.position = Vec3f{2.0f, 0.0f, 0.0f};
    int id_b = scene.AddObject(b);

    CHECK(id_a != id_b);
    CHECK(scene.objects.size() == 2);
    CHECK(scene.FindObject(id_a) != nullptr);
    CHECK(scene.FindObject(id_a)->name == "A");
    CHECK(scene.FindObject(id_b)->position.x == 2.0f);
    CHECK(scene.FindObject(-1) == nullptr);
    // One triangle per object referencing the shared mesh -- confirms
    // TotalTriangleCount sums per-*object* references, not per unique mesh.
    CHECK(scene.TotalTriangleCount() == 2);

    CHECK(scene.RemoveObject(id_a));
    CHECK(scene.FindObject(id_a) == nullptr);
    CHECK(!scene.RemoveObject(id_a));  // already gone -- removing again reports false
    CHECK(scene.objects.size() == 1);
    CHECK(scene.TotalTriangleCount() == 1);

    // --- MeshData::RemoveVertices (Phase 3 vertex editing) ---
    {
        // A quad in the XY plane, 2 triangles: (0,1,2) and (0,2,3).
        MeshData quad;
        quad.name = "quad";
        quad.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
        quad.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0, 1};
        quad.texcoords = {0, 0, 1, 0, 1, 1, 0, 1};
        quad.indices = {0, 1, 2, 0, 2, 3};
        CHECK(quad.VertexCount() == 4);
        CHECK(quad.TriangleCount() == 2);

        // Removing vertex 1 drops the only triangle that referenced it
        // (0,1,2), leaving just (0,2,3) -- remapped to (0,1,2) once
        // vertex 1's gone and the rest compact down by one.
        quad.RemoveVertices({1});
        CHECK(quad.VertexCount() == 3);
        CHECK(quad.TriangleCount() == 1);
        // Old vertex 2 (1,1,0) is now at new index 1.
        CHECK(quad.positions[3] == 1.0f && quad.positions[4] == 1.0f && quad.positions[5] == 0.0f);
        // Old vertex 3 (0,1,0) is now at new index 2.
        CHECK(quad.positions[6] == 0.0f && quad.positions[7] == 1.0f && quad.positions[8] == 0.0f);
        CHECK(quad.indices[0] == 0 && quad.indices[1] == 1 && quad.indices[2] == 2);
        CHECK(quad.normals.size() == 9);    // 3 vertices kept, still parallel to positions
        CHECK(quad.texcoords.size() == 6);  // ditto

        // Removing every remaining vertex leaves a valid, empty mesh.
        MeshData all_gone = quad;
        all_gone.RemoveVertices({0, 1, 2});
        CHECK(all_gone.VertexCount() == 0);
        CHECK(all_gone.TriangleCount() == 0);

        // Duplicate and out-of-range indices in the removal list are
        // harmless no-ops rather than corrupting anything.
        MeshData dup_test = quad;
        dup_test.RemoveVertices({0, 0, 99, -1});
        CHECK(dup_test.VertexCount() == 2);
        CHECK(dup_test.TriangleCount() == 0);  // the one remaining triangle referenced vertex 0
    }

    // --- MeshData::MergeVertices (Phase 3 vertex editing) ---
    {
        // A lone triangle: (0,0,0), (2,0,0), (0,2,0).
        MeshData tri;
        tri.name = "tri";
        tri.positions = {0, 0, 0, 2, 0, 0, 0, 2, 0};
        tri.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
        tri.texcoords = {0, 0, 1, 0, 0, 1};
        tri.indices = {0, 1, 2};

        // Merging two of a triangle's own corners collapses it to a
        // degenerate (zero-area) triangle, which is dropped rather than
        // kept -- the kept vertex lands at the averaged position, and the
        // vertex count drops by exactly one (the merged-away corner).
        MeshData collapsed = tri;
        collapsed.MergeVertices({0, 1});
        CHECK(collapsed.VertexCount() == 2);
        CHECK(collapsed.TriangleCount() == 0);
        // Kept vertex (lowest index, 0) is now the average of (0,0,0) and (2,0,0).
        CHECK(collapsed.positions[0] == 1.0f && collapsed.positions[1] == 0.0f && collapsed.positions[2] == 0.0f);
        // Old vertex 2 (0,2,0) compacted down to new index 1, untouched.
        CHECK(collapsed.positions[3] == 0.0f && collapsed.positions[4] == 2.0f && collapsed.positions[5] == 0.0f);
        CHECK(collapsed.normals.size() == 6);
        CHECK(collapsed.texcoords.size() == 4);

        // Two disconnected triangles sharing a duplicated seam vertex pair
        // (vertex 2 and vertex 3 sit at the same position) -- welding them
        // together stitches the seam without destroying either triangle,
        // since the merged vertex still leaves both triangles non-degenerate.
        MeshData seam;
        seam.name = "seam";
        seam.positions = {
            0, 0, 0,  // 0
            1, 0, 0,  // 1
            1, 1, 0,  // 2 (seam)
            1, 1, 0,  // 3 (seam, duplicate of 2)
            0, 1, 0,  // 4
        };
        seam.indices = {0, 1, 2, 3, 4, 0};  // triangle A: 0,1,2 ; triangle B: 3,4,0 -- seam is vertex 2 vs vertex 3
        CHECK(seam.VertexCount() == 5);
        CHECK(seam.TriangleCount() == 2);
        seam.MergeVertices({2, 3});
        CHECK(seam.VertexCount() == 4);   // one seam vertex welded away
        CHECK(seam.TriangleCount() == 2);  // both triangles survive, still non-degenerate
        // The kept vertex (index 2) is unchanged since both were at the same position.
        CHECK(seam.positions[6] == 1.0f && seam.positions[7] == 1.0f && seam.positions[8] == 0.0f);

        // Fewer than 2 distinct valid indices is a harmless no-op: a single
        // index, duplicates that collapse to one, and all-out-of-range all
        // leave the mesh completely unchanged.
        MeshData untouched = tri;
        untouched.MergeVertices({0});
        CHECK(untouched.VertexCount() == 3 && untouched.TriangleCount() == 1);
        untouched.MergeVertices({1, 1, 1});
        CHECK(untouched.VertexCount() == 3 && untouched.TriangleCount() == 1);
        untouched.MergeVertices({99, -1});
        CHECK(untouched.VertexCount() == 3 && untouched.TriangleCount() == 1);
    }

    // --- MeshData::RecalculateNormals (Phase 3 vertex editing) ---
    {
        // A flat quad in the XY plane facing +Z, deliberately seeded with
        // wrong normals -- recalculation should overwrite them with the
        // exact, analytically-known (0,0,1) for every vertex.
        MeshData quad;
        quad.name = "quad";
        quad.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
        quad.normals = {1, 0, 0, 1, 0, 0, 1, 0, 0, 1, 0, 0};  // deliberately wrong
        quad.indices = {0, 1, 2, 0, 2, 3};
        quad.RecalculateNormals();
        CHECK(quad.normals.size() == 12);
        for (int v = 0; v < 4; v++) {
            CHECK(quad.normals[static_cast<size_t>(v) * 3 + 0] == 0.0f);
            CHECK(quad.normals[static_cast<size_t>(v) * 3 + 1] == 0.0f);
            CHECK(quad.normals[static_cast<size_t>(v) * 3 + 2] == 1.0f);
        }

        // A "tent": two triangles sharing an edge (vertices 1,2), folded so
        // they're not coplanar -- the shared vertices' recalculated normal
        // should be the (normalized) average of both faces' own normals,
        // not either one alone. Triangle A (0,1,2) lies flat in the XY
        // plane (normal (0,0,1)); triangle B (1,2,3) is tilted up out of
        // that plane by folding vertex 3 upward off to the side.
        MeshData tent;
        tent.name = "tent";
        tent.positions = {
            0, 0, 0,  // 0
            1, 0, 0,  // 1 (shared edge)
            1, 1, 0,  // 2 (shared edge)
            2, 0, 1,  // 3 (folded up and out)
        };
        tent.indices = {0, 1, 2, 1, 3, 2};
        tent.RecalculateNormals();
        CHECK(tent.normals.size() == 12);
        // Vertex 0 (only in triangle A) keeps exactly triangle A's own flat normal.
        CHECK(tent.normals[0] == 0.0f && tent.normals[1] == 0.0f && tent.normals[2] == 1.0f);
        // Every normal must come out unit-length.
        for (int v = 0; v < 4; v++) {
            float x = tent.normals[static_cast<size_t>(v) * 3 + 0];
            float y = tent.normals[static_cast<size_t>(v) * 3 + 1];
            float z = tent.normals[static_cast<size_t>(v) * 3 + 2];
            float len = std::sqrt(x * x + y * y + z * z);
            CHECK(std::fabs(len - 1.0f) < 1e-4f);
        }
        // Vertex 1 is shared between the flat face A and the tilted face B,
        // so its normal must differ from either face's own normal alone --
        // proof it's actually averaging, not just picking one face.
        CHECK(std::fabs(tent.normals[3] - 0.0f) > 1e-4f || std::fabs(tent.normals[5] - 1.0f) > 1e-4f);

        // A vertex referenced by zero triangles gets a {0,0,1} fallback
        // instead of NaN or leftover garbage.
        MeshData orphan;
        orphan.name = "orphan";
        orphan.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 5, 5, 5};  // vertex 3 unreferenced
        orphan.indices = {0, 1, 2};
        orphan.RecalculateNormals();
        CHECK(orphan.normals[9] == 0.0f && orphan.normals[10] == 0.0f && orphan.normals[11] == 1.0f);

        // No triangles at all is a harmless no-op -- doesn't crash, doesn't
        // fabricate a normals array out of thin air.
        MeshData no_tris;
        no_tris.name = "no_tris";
        no_tris.positions = {0, 0, 0, 1, 0, 0};
        no_tris.RecalculateNormals();
        CHECK(no_tris.normals.empty());
    }

    // --- MeshData::SubdivideFaces (Phase 3 vertex editing) ---
    {
        // Same quad as the RemoveVertices/MergeVertices tests: (0,1,2) and
        // (0,2,3), both in the XY plane.
        auto make_quad = []() {
            MeshData quad;
            quad.name = "quad";
            quad.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
            quad.indices = {0, 1, 2, 0, 2, 3};
            return quad;
        };

        // All 4 vertices selected -- both triangles are "fully selected
        // faces" and each gets centroid-subdivided into 3.
        MeshData both = make_quad();
        std::vector<int> centroids = both.SubdivideFaces({0, 1, 2, 3});
        CHECK(centroids.size() == 2);
        CHECK(both.VertexCount() == 6);
        CHECK(both.TriangleCount() == 6);
        // Triangle 0's centroid (index 4): average of (0,0,0),(1,0,0),(1,1,0).
        CHECK(std::fabs(both.positions[12] - 2.0f / 3.0f) < 1e-5f);
        CHECK(std::fabs(both.positions[13] - 1.0f / 3.0f) < 1e-5f);
        CHECK(both.positions[14] == 0.0f);
        // Triangle 1's centroid (index 5): average of (0,0,0),(1,1,0),(0,1,0).
        CHECK(std::fabs(both.positions[15] - 1.0f / 3.0f) < 1e-5f);
        CHECK(std::fabs(both.positions[16] - 2.0f / 3.0f) < 1e-5f);
        CHECK(both.positions[17] == 0.0f);

        // Only vertices 0,1,2 selected -- triangle (0,1,2) is fully
        // selected and subdivides, but (0,2,3) isn't (3 is unselected) and
        // is left completely untouched.
        MeshData partial = make_quad();
        std::vector<int> one_centroid = partial.SubdivideFaces({0, 1, 2});
        CHECK(one_centroid.size() == 1);
        CHECK(partial.VertexCount() == 5);
        CHECK(partial.TriangleCount() == 4);  // 3 new + the 1 untouched original

        // Nothing selected is a harmless no-op.
        MeshData untouched = make_quad();
        CHECK(untouched.SubdivideFaces({}).empty());
        CHECK(untouched.VertexCount() == 4 && untouched.TriangleCount() == 2);
    }

    // --- MeshData::ExtrudeFaces (Phase 3 vertex editing) ---
    {
        MeshData quad;
        quad.name = "quad";
        quad.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
        quad.indices = {0, 1, 2, 0, 2, 3};

        std::vector<int> cap = quad.ExtrudeFaces({0, 1, 2, 3}, 2.0f);
        CHECK(cap.size() == 4);
        CHECK((cap == std::vector<int>{4, 5, 6, 7}));
        CHECK(quad.VertexCount() == 8);
        // 2 cap triangles (remapped to the new ring) + 4 boundary edges * 2
        // wall triangles each = 10; the shared diagonal (0,2)/(2,0) is
        // correctly NOT walled, since it's internal to the selected face.
        CHECK(quad.TriangleCount() == 10);
        // The 4 duplicated cap vertices sit exactly `distance` along the
        // quad's own +Z normal from their originals.
        for (size_t i = 0; i < 4; i++) {
            CHECK(quad.positions[(4 + i) * 3 + 0] == quad.positions[i * 3 + 0]);
            CHECK(quad.positions[(4 + i) * 3 + 1] == quad.positions[i * 3 + 1]);
            CHECK(std::fabs(quad.positions[(4 + i) * 3 + 2] - 2.0f) < 1e-5f);
        }
        // The cap triangles now reference the new ring, not the original.
        CHECK(quad.indices[0] == 4 && quad.indices[1] == 5 && quad.indices[2] == 6);
        CHECK(quad.indices[3] == 4 && quad.indices[4] == 6 && quad.indices[5] == 7);
        // A wall triangle for the (0,1) boundary edge exists, connecting
        // the untouched original ring to the new one.
        auto has_triangle = [&](unsigned int ta, unsigned int tb, unsigned int tc) {
            for (size_t t = 0; t + 2 < quad.indices.size(); t += 3) {
                if (quad.indices[t] == ta && quad.indices[t + 1] == tb && quad.indices[t + 2] == tc) return true;
            }
            return false;
        };
        CHECK(has_triangle(0, 1, 5));
        CHECK(has_triangle(0, 5, 4));
        // The shared diagonal was never walled (would show up as (0,2,6) / (0,6,4) or similar).
        CHECK(!has_triangle(0, 2, 6));

        // Nothing selected is a harmless no-op.
        MeshData empty_sel = quad;
        CHECK(empty_sel.ExtrudeFaces({}, 1.0f).empty());
        CHECK(empty_sel.VertexCount() == 8 && empty_sel.TriangleCount() == 10);
    }

    // --- MeshData::DissolveVertex (Phase 3 vertex editing) ---
    {
        // A flat hexagon fan: center vertex 0, ring vertices 1..6, 6
        // triangles all sharing vertex 0 -- a clean, closed fan around it.
        MeshData fan;
        fan.name = "fan";
        fan.positions = {
            0, 0, 0,  // 0: center
            1, 0, 0,  // 1
            0.5f, 0.866f, 0,  // 2
            -0.5f, 0.866f, 0,  // 3
            -1, 0, 0,  // 4
            -0.5f, -0.866f, 0,  // 5
            0.5f, -0.866f, 0,  // 6
        };
        fan.indices = {0, 1, 2, 0, 2, 3, 0, 3, 4, 0, 4, 5, 0, 5, 6, 0, 6, 1};
        CHECK(fan.VertexCount() == 7);
        CHECK(fan.TriangleCount() == 6);
        CHECK(fan.DissolveVertex(0));
        // Center removed and the 6-triangle fan around it replaced with a
        // clean (N-2)=4-triangle fan of the remaining ring -- no hole.
        CHECK(fan.VertexCount() == 6);
        CHECK(fan.TriangleCount() == 4);

        // A quad's corner with only 1 incident triangle can't form a
        // closed ring around it -- falls back to a plain hole-leaving
        // removal instead of a malformed retriangulation.
        MeshData quad;
        quad.name = "quad";
        quad.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
        quad.indices = {0, 1, 2, 0, 2, 3};
        CHECK(quad.DissolveVertex(1));
        CHECK(quad.VertexCount() == 3);
        CHECK(quad.TriangleCount() == 1);  // same result RemoveVertices({1}) alone would give

        // Out-of-range index is a clean no-op, reported as such.
        MeshData untouched = quad;
        CHECK(!untouched.DissolveVertex(99));
        CHECK(!untouched.DissolveVertex(-1));
        CHECK(untouched.VertexCount() == 3 && untouched.TriangleCount() == 1);
    }

    // --- MeshData::InsetFaces (Phase 3 vertex editing) ---
    {
        MeshData quad;
        quad.name = "quad";
        quad.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0, 0, 1, 0};
        quad.indices = {0, 1, 2, 0, 2, 3};

        MeshData half = quad;
        std::vector<int> cap = half.InsetFaces({0, 1, 2, 3}, 0.5f);
        CHECK(cap.size() == 4);
        CHECK((cap == std::vector<int>{4, 5, 6, 7}));
        CHECK(half.VertexCount() == 8);
        CHECK(half.TriangleCount() == 10);  // same topology shape as ExtrudeFaces' own quad test
        // Each duplicate sits exactly halfway between its original and the
        // group's centroid (0.5, 0.5, 0).
        CHECK(std::fabs(half.positions[12] - 0.25f) < 1e-5f && std::fabs(half.positions[13] - 0.25f) < 1e-5f);
        CHECK(std::fabs(half.positions[15] - 0.75f) < 1e-5f && std::fabs(half.positions[16] - 0.25f) < 1e-5f);
        CHECK(std::fabs(half.positions[18] - 0.75f) < 1e-5f && std::fabs(half.positions[19] - 0.75f) < 1e-5f);
        CHECK(std::fabs(half.positions[21] - 0.25f) < 1e-5f && std::fabs(half.positions[22] - 0.75f) < 1e-5f);
        // Every duplicate stays exactly in-plane (no lift along any normal, unlike ExtrudeFaces).
        for (size_t i = 4; i < 8; i++) CHECK(half.positions[i * 3 + 2] == 0.0f);

        // amount=1 fully collapses every duplicate onto the centroid.
        MeshData full = quad;
        full.InsetFaces({0, 1, 2, 3}, 1.0f);
        for (size_t i = 4; i < 8; i++) {
            CHECK(std::fabs(full.positions[i * 3 + 0] - 0.5f) < 1e-5f);
            CHECK(std::fabs(full.positions[i * 3 + 1] - 0.5f) < 1e-5f);
        }

        // amount=0 leaves every duplicate exactly where the original was
        // (a degenerate, zero-width inset, but still a valid one).
        MeshData zero = quad;
        zero.InsetFaces({0, 1, 2, 3}, 0.0f);
        for (int i = 0; i < 4; i++) {
            CHECK(zero.positions[static_cast<size_t>(4 + i) * 3 + 0] == zero.positions[static_cast<size_t>(i) * 3 + 0]);
            CHECK(zero.positions[static_cast<size_t>(4 + i) * 3 + 1] == zero.positions[static_cast<size_t>(i) * 3 + 1]);
        }

        // Out-of-[0,1] amounts are clamped rather than extrapolating past the centroid or the original.
        MeshData clamped_hi = quad;
        clamped_hi.InsetFaces({0, 1, 2, 3}, 1.5f);
        CHECK(std::fabs(clamped_hi.positions[4 * 3 + 0] - 0.5f) < 1e-5f);
        MeshData clamped_lo = quad;
        clamped_lo.InsetFaces({0, 1, 2, 3}, -0.5f);
        CHECK(clamped_lo.positions[4 * 3 + 0] == clamped_lo.positions[0 * 3 + 0]);

        // Nothing selected is a harmless no-op.
        MeshData untouched = quad;
        CHECK(untouched.InsetFaces({}, 0.5f).empty());
        CHECK(untouched.VertexCount() == 4 && untouched.TriangleCount() == 2);
    }

    // --- MeshData::AddVertex / MakeFace (Phase 3 vertex editing) ---
    {
        MeshData mesh;
        mesh.name = "scratch";
        CHECK(mesh.VertexCount() == 0);

        int v0 = mesh.AddVertex(0, 0, 0);
        int v1 = mesh.AddVertex(1, 0, 0);
        int v2 = mesh.AddVertex(1, 1, 0);
        int v3 = mesh.AddVertex(0, 1, 0);
        CHECK(v0 == 0 && v1 == 1 && v2 == 2 && v3 == 3);
        CHECK(mesh.VertexCount() == 4);
        CHECK(mesh.TriangleCount() == 0);  // isolated -- nothing references them yet
        CHECK(mesh.positions[static_cast<size_t>(v2) * 3 + 0] == 1.0f && mesh.positions[static_cast<size_t>(v2) * 3 + 1] == 1.0f);

        // Fan-triangulates from the first vertex: a quad becomes 2 triangles.
        CHECK(mesh.MakeFace({v0, v1, v2, v3}));
        CHECK(mesh.TriangleCount() == 2);
        CHECK(mesh.indices[0] == 0 && mesh.indices[1] == 1 && mesh.indices[2] == 2);
        CHECK(mesh.indices[3] == 0 && mesh.indices[4] == 2 && mesh.indices[5] == 3);

        // Fewer than 3 distinct valid vertices is a harmless no-op.
        MeshData too_few = mesh;
        CHECK(!too_few.MakeFace({v0, v1}));
        CHECK(!too_few.MakeFace({v0, v0, v0, 99, -1}));  // dedupes/filters down to just {v0}
        CHECK(too_few.TriangleCount() == 2);  // unchanged

        // Duplicate/out-of-range indices are filtered before checking the
        // 3-distinct-vertices minimum, same convention as the other ops.
        MeshData dup_test = mesh;
        CHECK(dup_test.MakeFace({v0, v0, v1, 99, v2, -1}));
        CHECK(dup_test.TriangleCount() == 3);  // one new triangle (v0,v1,v2) appended

        // AddVertex keeps normals/texcoords parallel to positions when the
        // mesh already has them -- a default {0,0,1} normal, {0,0} uv.
        MeshData with_attrs;
        with_attrs.positions = {0, 0, 0};
        with_attrs.normals = {0, 1, 0};
        with_attrs.texcoords = {0.5f, 0.5f};
        int new_v = with_attrs.AddVertex(5, 5, 5);
        CHECK(new_v == 1);
        CHECK(with_attrs.normals.size() == 6);
        CHECK(with_attrs.normals[3] == 0.0f && with_attrs.normals[4] == 0.0f && with_attrs.normals[5] == 1.0f);
        CHECK(with_attrs.texcoords.size() == 4);
        CHECK(with_attrs.texcoords[2] == 0.0f && with_attrs.texcoords[3] == 0.0f);

        // A realistic build-from-scratch workflow: an existing triangle
        // plus one freshly-added vertex, connected into a second triangle
        // that reuses one of the original triangle's own edges.
        MeshData tri;
        tri.positions = {0, 0, 0, 1, 0, 0, 1, 1, 0};
        tri.indices = {0, 1, 2};
        int v3b = tri.AddVertex(0, 1, 0);
        CHECK(tri.MakeFace({0, 2, static_cast<int>(v3b)}));
        CHECK(tri.VertexCount() == 4);
        CHECK(tri.TriangleCount() == 2);
    }

    // --- MeshData::MergeByDistance (Phase 3 vertex editing) ---
    {
        // 3 exact-duplicate vertices (a cube-corner-style raylib
        // unwelded seam), each used by its own separate triangle, plus 6
        // unrelated, well-separated vertices.
        MeshData mesh;
        mesh.name = "seam";
        mesh.positions = {
            0.5f, 0.5f, 0.5f,  // 0 (dup group)
            0.5f, 0.5f, 0.5f,  // 1 (dup group)
            0.5f, 0.5f, 0.5f,  // 2 (dup group)
            1, 0, 0,           // 3
            0, 1, 0,           // 4
            2, 0, 0,           // 5
            0, 2, 0,           // 6
            3, 0, 0,           // 7
            0, 3, 0,           // 8
        };
        mesh.indices = {0, 3, 4, 1, 5, 6, 2, 7, 8};
        CHECK(mesh.VertexCount() == 9);
        CHECK(mesh.TriangleCount() == 3);

        int removed = mesh.MergeByDistance(0.0f);
        CHECK(removed == 2);
        CHECK(mesh.VertexCount() == 7);
        CHECK(mesh.TriangleCount() == 3);  // none degenerate -- each triangle only touched 1 of the 3 dupes
        // Kept vertex (index 0, the group's lowest) is unchanged since all 3 were identical.
        CHECK(mesh.positions[0] == 0.5f && mesh.positions[1] == 0.5f && mesh.positions[2] == 0.5f);
        CHECK(mesh.indices[0] == 0 && mesh.indices[3] == 0 && mesh.indices[6] == 0);  // all 3 now reference root 0

        // Transitive grouping through a chain: A-B and B-C are each within
        // threshold, but A-C alone isn't -- they should still end up one
        // group, averaged.
        MeshData chain;
        chain.name = "chain";
        chain.positions = {5, 0, 0, 5, 0, 0.05f, 5, 0, 0.1f};
        int chain_removed = chain.MergeByDistance(0.06f);
        CHECK(chain_removed == 2);
        CHECK(chain.VertexCount() == 1);
        CHECK(std::fabs(chain.positions[2] - 0.05f) < 1e-5f);  // average of 0, 0.05, 0.1

        // A merged pair that both belong to the *same* triangle produces a
        // degenerate result, dropped same as MergeVertices.
        MeshData degen;
        degen.name = "degen";
        degen.positions = {0, 0, 0, 0, 0, 0, 1, 1, 0};
        degen.indices = {0, 1, 2};
        CHECK(degen.MergeByDistance(0.0f) == 1);
        CHECK(degen.VertexCount() == 2);
        CHECK(degen.TriangleCount() == 0);

        // Nothing within threshold of anything else is a harmless no-op.
        MeshData untouched;
        untouched.positions = {0, 0, 0, 10, 0, 0, 0, 10, 0};
        CHECK(untouched.MergeByDistance(0.5f) == 0);
        CHECK(untouched.VertexCount() == 3);
    }

    // --- MeshData::FlipNormals (Phase 3 vertex editing) ---
    {
        MeshData tri;
        tri.name = "tri";
        tri.positions = {0, 0, 0, 1, 0, 0, 0, 1, 0};
        tri.normals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
        tri.indices = {0, 1, 2};
        tri.FlipNormals();
        // Winding reversed: 2nd/3rd corners swapped.
        CHECK(tri.indices[0] == 0 && tri.indices[1] == 2 && tri.indices[2] == 1);
        // Every normal negated.
        for (int i = 0; i < 3; i++) {
            CHECK(tri.normals[static_cast<size_t>(i) * 3 + 2] == -1.0f);
        }

        // No triangles/normals is a harmless no-op (doesn't crash).
        MeshData empty;
        empty.FlipNormals();
        CHECK(empty.VertexCount() == 0 && empty.TriangleCount() == 0);
    }

    // --- EnsureUniqueMesh (Phase 3 vertex editing) ---
    {
        Scene shared;
        int mesh_idx = static_cast<int>(shared.meshes.size());
        shared.meshes.push_back(MakeTriangle());
        Object3D a_obj;
        a_obj.name = "A";
        a_obj.mesh_index = mesh_idx;
        int a_id = shared.AddObject(a_obj);
        Object3D b_obj;
        b_obj.name = "B";
        b_obj.mesh_index = mesh_idx;  // shares A's mesh
        int b_id = shared.AddObject(b_obj);

        size_t mesh_count_before = shared.meshes.size();
        int a_new_index = shared.EnsureUniqueMesh(a_id);
        CHECK(a_new_index != mesh_idx);  // A and B still share -- must have cloned
        CHECK(shared.meshes.size() == mesh_count_before + 1);
        CHECK(shared.FindObject(a_id)->mesh_index == a_new_index);
        CHECK(shared.FindObject(b_id)->mesh_index == mesh_idx);  // B untouched
        CHECK(shared.meshes[static_cast<size_t>(a_new_index)].positions == shared.meshes[static_cast<size_t>(mesh_idx)].positions);

        // Now A is exclusively its own -- a second call is a no-op.
        int a_index_again = shared.EnsureUniqueMesh(a_id);
        CHECK(a_index_again == a_new_index);
        CHECK(shared.meshes.size() == mesh_count_before + 1);  // no new clone

        CHECK(shared.EnsureUniqueMesh(-1) == -1);  // nonexistent object
        Object3D group_obj;
        group_obj.name = "Group";
        group_obj.mesh_index = -1;  // empty group node
        int group_id = shared.AddObject(group_obj);
        CHECK(shared.EnsureUniqueMesh(group_id) == -1);  // no mesh to clone
    }

    // --- Descendants (Phase 3 parenting/grouping) ---
    {
        Scene tree;
        Object3D root_obj;
        root_obj.name = "root";
        int root = tree.AddObject(root_obj);
        Object3D child1_obj;
        child1_obj.name = "child1";
        child1_obj.parent = root;
        int child1 = tree.AddObject(child1_obj);
        Object3D child2_obj;
        child2_obj.name = "child2";
        child2_obj.parent = root;
        int child2 = tree.AddObject(child2_obj);
        Object3D grandchild_obj;
        grandchild_obj.name = "grandchild";
        grandchild_obj.parent = child1;
        int grandchild = tree.AddObject(grandchild_obj);
        Object3D unrelated_obj;
        unrelated_obj.name = "unrelated";
        int unrelated = tree.AddObject(unrelated_obj);
        (void)unrelated;

        std::vector<int> desc = tree.Descendants(root);
        CHECK(desc.size() == 3);
        CHECK(std::find(desc.begin(), desc.end(), child1) != desc.end());
        CHECK(std::find(desc.begin(), desc.end(), child2) != desc.end());
        CHECK(std::find(desc.begin(), desc.end(), grandchild) != desc.end());
        CHECK(tree.Descendants(child1).size() == 1);  // just grandchild
        CHECK(tree.Descendants(grandchild).empty());  // leaf node
        CHECK(tree.Descendants(unrelated).empty());

        // A corrupted/cyclic parent chain (shouldn't occur via
        // Editor::Model3DSetParent's own cycle check, but Descendants
        // itself must not infinite-loop if it ever does) terminates via
        // the visited-set guard instead of hanging.
        Object3D *root_ptr = tree.FindObject(root);
        root_ptr->parent = grandchild;  // root -> ... -> grandchild -> root
        std::vector<int> cyclic_desc = tree.Descendants(root);
        CHECK(cyclic_desc.size() <= 4);  // terminates; exact membership isn't the point here
    }

    // --- Extension checks ---
    CHECK(IsModel3DPath("thing.obj"));
    CHECK(IsModel3DPath("thing.GLTF"));
    CHECK(IsModel3DPath("path/to/thing.glb"));
    CHECK(IsModel3DPath("thing.iqm"));
    CHECK(IsModel3DPath("thing.vox"));
    CHECK(IsModel3DPath("thing.m3d"));
    CHECK(!IsModel3DPath("thing.blend"));  // handled by model3d_blend_import.h instead
    CHECK(!IsModel3DPath("thing.png"));
    CHECK(!IsModel3DPath("noextension"));

    // --- glTF export ---
    // Re-add an object so the exported scene has one visible, one hidden.
    Object3D c;
    c.name = "Hidden";
    c.mesh_index = mesh_index;
    c.visible = false;
    c.rotation_deg = Vec3f{0.0f, 90.0f, 0.0f};
    scene.AddObject(c);

    char tmpl[] = "/tmp/mep-model3d-doc-test-XXXXXX.gltf";
    int fd = mkstemps(tmpl, 5);
    CHECK(fd >= 0);
    close(fd);
    std::string path = tmpl;

    std::string error;
    CHECK(SaveModel3DGltf(scene, path, &error));

    std::ifstream in(path, std::ios::binary);
    CHECK(static_cast<bool>(in));
    std::stringstream ss;
    ss << in.rdbuf();
    std::string text = ss.str();
    std::filesystem::remove(path);

    Json doc;
    CHECK(Json::Parse(text, &doc));
    CHECK(doc.get("asset").get("version").as_string() == "2.0");
    CHECK(doc.get("nodes").items().size() == 2);  // "B" and "Hidden" -- "A" was removed above
    CHECK(doc.get("buffers").items().size() == 1);
    const std::string &uri = doc.get("buffers").items()[0].get("uri").as_string();
    CHECK(uri.rfind("data:application/octet-stream;base64,", 0) == 0);
    // Every node should carry its own name/extras/mesh -- confirms the
    // "one glTF mesh+material per Object3D" design (not shared, so hidden
    // vs. visible and each object's own color both survive independently
    // even though both objects reference the same Scene::meshes entry).
    bool found_hidden = false, found_b = false;
    for (const Json &node : doc.get("nodes").items()) {
        CHECK(node.contains("mesh"));
        if (node.get("name").as_string() == "Hidden") {
            found_hidden = true;
            CHECK(!node.get("extras").get("visible").as_bool(true));
        } else if (node.get("name").as_string() == "B") {
            found_b = true;
            CHECK(node.get("extras").get("visible").as_bool(false));
        }
    }
    CHECK(found_hidden && found_b);
    // No textured objects in `scene` -- confirms the exporter omits
    // "images"/"textures" entirely rather than emitting empty arrays.
    CHECK(!doc.contains("images"));
    CHECK(!doc.contains("textures"));

    // --- Texture loading + glTF embedding (Phase 3 materials/textures) ---
    // LoadImage/ExportImage/GenImageColor are pure CPU (stb_image/
    // stb_image_write) -- no GL context needed, safe in this windowless
    // binary, unlike LoadModel3DFile/AddPrimitiveToScene's own
    // GPU-upload-dependent halves.
    {
        Image gen_img = GenImageColor(4, 4, RED);
        char tex_tmpl[] = "/tmp/mep-model3d-doc-test-tex-XXXXXX.png";
        int tex_fd = mkstemps(tex_tmpl, 4);
        CHECK(tex_fd >= 0);
        close(tex_fd);
        CHECK(ExportImage(gen_img, tex_tmpl));
        UnloadImage(gen_img);

        Scene tex_scene;
        std::string tex_error;
        int tex_index = LoadTextureIntoScene(&tex_scene, tex_tmpl, &tex_error);
        std::filesystem::remove(tex_tmpl);
        CHECK(tex_index == 0);
        CHECK(tex_scene.textures.size() == 1);
        CHECK(tex_scene.textures[0].width == 4);
        CHECK(tex_scene.textures[0].height == 4);
        CHECK(tex_scene.textures[0].pixels.size() == 4 * 4 * 4);
        // A missing file fails cleanly instead of crashing.
        CHECK(LoadTextureIntoScene(&tex_scene, "/no/such/file.png", &tex_error) == -1);

        tex_scene.meshes.push_back(MakeTriangle());
        Object3D tex_obj;
        tex_obj.name = "Textured";
        tex_obj.mesh_index = 0;
        tex_obj.texture_index = tex_index;
        tex_scene.AddObject(tex_obj);

        char tex_gltf_tmpl[] = "/tmp/mep-model3d-doc-test-tex-XXXXXX.gltf";
        int tex_gltf_fd = mkstemps(tex_gltf_tmpl, 5);
        CHECK(tex_gltf_fd >= 0);
        close(tex_gltf_fd);
        std::string tex_gltf_error;
        CHECK(SaveModel3DGltf(tex_scene, tex_gltf_tmpl, &tex_gltf_error));
        std::ifstream tex_in(tex_gltf_tmpl);
        std::stringstream tex_ss;
        tex_ss << tex_in.rdbuf();
        std::filesystem::remove(tex_gltf_tmpl);
        Json tex_doc;
        CHECK(Json::Parse(tex_ss.str(), &tex_doc));
        CHECK(tex_doc.get("images").items().size() == 1);
        CHECK(tex_doc.get("textures").items().size() == 1);
        const std::string &img_uri = tex_doc.get("images").items()[0].get("uri").as_string();
        CHECK(img_uri.rfind("data:image/png;base64,", 0) == 0);
        CHECK(tex_doc.get("materials").items()[0].get("pbrMetallicRoughness").contains("baseColorTexture"));
    }

    // A scene with zero objects should still export a structurally valid
    // (empty) document rather than failing.
    Scene empty_scene;
    std::string empty_path = tmpl;  // reuse the same (now-removed) temp path
    CHECK(SaveModel3DGltf(empty_scene, empty_path, &error));
    std::ifstream in2(empty_path);
    std::stringstream ss2;
    ss2 << in2.rdbuf();
    Json empty_doc;
    CHECK(Json::Parse(ss2.str(), &empty_doc));
    CHECK(empty_doc.get("nodes").items().empty());
    std::filesystem::remove(empty_path);

    std::printf("model3d_doc_test passed\n");
    return 0;
}
