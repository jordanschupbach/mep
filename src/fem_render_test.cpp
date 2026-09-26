// Part J.2: result visualisation, verified.
//
// HOW YOU TEST A PICTURE. Not by looking at it. Every routine here
// produces geometry, and geometry has measurable properties that follow
// from what it is supposed to be: a closed surface encloses a known
// volume, an isosurface of a radius field is a sphere of known area, a
// plane section of a cube has a known cross-section, a watertight mesh
// has every edge shared by exactly two triangles. Those are the tests. A
// render that passes them can still look wrong, but every way it could
// look wrong that matters -- holes, inverted faces, missing elements,
// wrong colours -- fails one of them.

#include "fem_render.h"

#include "fem_elem.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <utility>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char *what, int line) {
    ++checks;
    if (ok) return;
    std::fflush(stdout);
    std::printf("CHECK FAILED: %s at %s:%d\n", what, __FILE__, line);
    ++failures;
}

#define CHECK(x) Check((x), #x, __LINE__)

using cad::Vec3d;
using fem::AnalysisModel;
using fem::BoundElement;
using fem::ElementShape;
using fem::RenderMesh;
using fem::RenderOptions;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }
double D(float v) { return static_cast<double>(v); }
bool Near(double a, double b, double tolerance) { return std::fabs(a - b) <= tolerance; }

AnalysisModel Cube(ElementShape shape, int m) {
    AnalysisModel model;
    fem::StudyMaterial material;
    material.youngs_modulus = fem::MaterialCurve::Constant(210e9);
    material.poissons_ratio = fem::MaterialCurve::Constant(0.3);
    model.materials.push_back(material);
    const int side = m + 1;
    for (int k = 0; k < side; ++k) {
        for (int j = 0; j < side; ++j) {
            for (int i = 0; i < side; ++i) {
                model.nodes.push_back(Vec3d{static_cast<double>(i) / m, static_cast<double>(j) / m,
                                            static_cast<double>(k) / m});
            }
        }
    }
    auto at = [&](int i, int j, int k) { return (k * side + j) * side + i; };
    for (int k = 0; k < m; ++k) {
        for (int j = 0; j < m; ++j) {
            for (int i = 0; i < m; ++i) {
                const int h[8] = {at(i, j, k),         at(i + 1, j, k),
                                  at(i + 1, j + 1, k), at(i, j + 1, k),
                                  at(i, j, k + 1),     at(i + 1, j, k + 1),
                                  at(i + 1, j + 1, k + 1), at(i, j + 1, k + 1)};
                if (shape == ElementShape::Hex8) {
                    BoundElement element;
                    element.shape = shape;
                    element.nodes = {h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]};
                    model.elements.push_back(element);
                    continue;
                }
                const int split[6][4] = {{h[0], h[1], h[2], h[6]}, {h[0], h[2], h[3], h[6]},
                                         {h[0], h[3], h[7], h[6]}, {h[0], h[7], h[4], h[6]},
                                         {h[0], h[4], h[5], h[6]}, {h[0], h[5], h[1], h[6]}};
                for (const auto &tet : split) {
                    BoundElement element;
                    element.shape = ElementShape::Tet4;
                    element.nodes = {tet[0], tet[1], tet[2], tet[3]};
                    model.elements.push_back(element);
                }
            }
        }
    }
    if (shape != ElementShape::Tet10) return model;
    static const int edges[6][2] = {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}};
    std::map<std::pair<int, int>, int> middles;
    for (BoundElement &element : model.elements) {
        const std::vector<int> corners = element.nodes;
        element.shape = ElementShape::Tet10;
        for (const auto &edge : edges) {
            const int u = corners[Idx(edge[0])];
            const int v = corners[Idx(edge[1])];
            const auto key = u < v ? std::make_pair(u, v) : std::make_pair(v, u);
            const auto found = middles.find(key);
            if (found != middles.end()) {
                element.nodes.push_back(found->second);
                continue;
            }
            const int index = model.NodeCount();
            model.nodes.push_back((model.nodes[Idx(u)] + model.nodes[Idx(v)]) * 0.5);
            middles[key] = index;
            element.nodes.push_back(index);
        }
    }
    return model;
}

// Every edge of a closed triangle mesh is shared by exactly two
// triangles. THE PROPERTY MARCHING CUBES FAMOUSLY LOSES and marching
// tetrahedra cannot, so it is worth checking rather than assuming: a
// surface with a single unmatched edge has a hole, and a hole is
// invisible from most angles.
bool Watertight(const RenderMesh &mesh, int *out_unmatched) {
    std::map<std::pair<int, int>, int> edges;
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const int v[3] = {static_cast<int>(mesh.indices[t]), static_cast<int>(mesh.indices[t + 1]),
                          static_cast<int>(mesh.indices[t + 2])};
        for (int i = 0; i < 3; ++i) {
            int a = v[i];
            int b = v[(i + 1) % 3];
            // Keyed on *position*, not on index: marching tetrahedra
            // makes a fresh vertex for every tet that crosses an edge, so
            // two tets sharing a face produce two distinct indices at the
            // same point. Keying on the index would report every
            // isosurface as full of holes.
            if (a > b) std::swap(a, b);
            ++edges[{a, b}];
        }
    }
    *out_unmatched = 0;
    for (const auto &entry : edges) {
        if (entry.second != 2) ++*out_unmatched;
    }
    return *out_unmatched == 0;
}

// The same, but merging vertices that sit at the same point first.
bool WatertightByPosition(const RenderMesh &mesh, double tolerance, int *out_unmatched) {
    std::vector<int> merged(mesh.positions.size() / 3);
    std::map<std::array<long long, 3>, int> grid;
    for (std::size_t i = 0; i < merged.size(); ++i) {
        const std::array<long long, 3> key{
            llround(D(mesh.positions[i * 3]) / tolerance),
            llround(D((mesh.positions[i * 3 + 1])) / tolerance),
            llround(D((mesh.positions[i * 3 + 2])) / tolerance)};
        const auto found = grid.find(key);
        if (found != grid.end()) {
            merged[i] = found->second;
            continue;
        }
        merged[i] = static_cast<int>(i);
        grid[key] = static_cast<int>(i);
    }
    std::map<std::pair<int, int>, int> edges;
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const int v[3] = {merged[mesh.indices[t]], merged[mesh.indices[t + 1]],
                          merged[mesh.indices[t + 2]]};
        for (int i = 0; i < 3; ++i) {
            int a = v[i];
            int b = v[(i + 1) % 3];
            if (a == b) continue;  // a degenerate sliver, which is not a hole
            if (a > b) std::swap(a, b);
            ++edges[{a, b}];
        }
    }
    *out_unmatched = 0;
    for (const auto &entry : edges) {
        if (entry.second != 2) ++*out_unmatched;
    }
    return *out_unmatched == 0;
}

std::vector<Vec3d> NoDisplacement(const AnalysisModel &model) {
    return std::vector<Vec3d>(model.nodes.size(), Vec3d{});
}

// --- The decomposition -----------------------------------------------------

void TestDecomposition() {
    std::printf("every element as tetrahedra:\n");
    // THE VOLUME IS THE TEST. A split that misses a corner, overlaps
    // itself or inverts a tetrahedron gets the volume wrong, and nothing
    // else about a decomposition is as easy to check or as hard to fake.
    for (const ElementShape shape :
         {ElementShape::Tet4, ElementShape::Tet10, ElementShape::Hex8}) {
        AnalysisModel model = Cube(shape, 3);
        fem::TetView view;
        std::string error;
        CHECK(fem::Decompose(model, &view, &error));
        double volume = 0.0;
        int inverted = 0;
        for (const auto &tet : view.tets) {
            const Vec3d a = model.nodes[Idx(tet[0])], b = model.nodes[Idx(tet[1])];
            const Vec3d c = model.nodes[Idx(tet[2])], d = model.nodes[Idx(tet[3])];
            const double v = (b - a).Cross(c - a).Dot(d - a) / 6.0;
            volume += v;
            if (v <= 0.0) ++inverted;
        }
        std::printf("  %-6s %4d elements -> %5d tets, volume %.15f, %d inverted\n",
                    fem::ElementShapeName(shape), model.ElementCount(),
                    static_cast<int>(view.tets.size()), volume, inverted);
        CHECK(Near(volume, 1.0, 1e-12));
        CHECK(inverted == 0);
        CHECK(view.element_of_tet.size() == view.tets.size());
    }

    // And the boundary of the decomposition is the boundary of the model:
    // six unit squares, and nothing from the inside.
    for (const ElementShape shape :
         {ElementShape::Tet4, ElementShape::Tet10, ElementShape::Hex8}) {
        AnalysisModel model = Cube(shape, 3);
        fem::TetView view;
        std::vector<std::array<int, 3>> triangles;
        std::string error;
        CHECK(fem::Decompose(model, &view, &error));
        CHECK(fem::BoundaryTriangles(model, view, &triangles, &error));
        double area = 0.0;
        double enclosed = 0.0;
        for (const auto &triangle : triangles) {
            const Vec3d a = model.nodes[Idx(triangle[0])], b = model.nodes[Idx(triangle[1])],
                        c = model.nodes[Idx(triangle[2])];
            area += 0.5 * (b - a).Cross(c - a).Length();
            enclosed += a.Dot(b.Cross(c)) / 6.0;
        }
        std::printf("  %-6s boundary: %4d triangles, area %.15f, encloses %.15f\n",
                    fem::ElementShapeName(shape), static_cast<int>(triangles.size()), area,
                    enclosed);
        CHECK(Near(area, 6.0, 1e-12));
        // POSITIVE, which is the whole content of the outward-winding
        // rule: get one face backwards and this falls by twice that
        // face's contribution while the area does not move at all.
        CHECK(Near(enclosed, 1.0, 1e-12));
    }
}

// --- The surface -----------------------------------------------------------

void TestSurface() {
    std::printf("the drawable surface:\n");
    AnalysisModel model = Cube(ElementShape::Hex8, 4);
    std::vector<Vec3d> displacement(model.nodes.size());
    for (int node = 0; node < model.NodeCount(); ++node) {
        // A twist, so that the deformed shape is not a scaled copy of the
        // original and an ignored displacement would be obvious.
        const Vec3d &p = model.nodes[Idx(node)];
        displacement[Idx(node)] = Vec3d{-p.y * p.z * 1e-5, p.x * p.z * 1e-5, 0.0};
    }
    std::vector<double> field(model.nodes.size());
    for (int node = 0; node < model.NodeCount(); ++node) field[Idx(node)] = model.nodes[Idx(node)].z;

    RenderOptions options;
    options.undeformed = true;
    RenderMesh mesh;
    std::string error;
    CHECK(fem::RenderSurface(model, displacement, field, options, &mesh, &error));
    int unmatched = 0;
    std::printf("  %d vertices, %d triangles, area %.12f, encloses %.12f\n", mesh.vertex_count,
                mesh.triangle_count, mesh.area, mesh.enclosed_volume);
    CHECK(Watertight(mesh, &unmatched));
    CHECK(Near(mesh.area, 6.0, 1e-12));
    CHECK(Near(mesh.enclosed_volume, 1.0, 1e-12));
    CHECK(Near(mesh.field_min, 0.0, 1e-15));
    CHECK(Near(mesh.field_max, 1.0, 1e-15));
    // Indexed, not de-indexed: the surface of a 4-per-side cube has 98
    // nodes, and three times as many vertices would mean every triangle
    // carried its own copy and the colours could not interpolate.
    CHECK(mesh.vertex_count == 98);
    CHECK(mesh.vertex_count * 3 == static_cast<int>(mesh.positions.size()));
    CHECK(mesh.vertex_count * 4 == static_cast<int>(mesh.colors.size()));

    // Normals: unit length, and pointing out of a convex body, which for
    // a cube means agreeing with the direction from the centre.
    double worst_length = 0.0;
    double worst_outward = 1.0;
    for (int v = 0; v < mesh.vertex_count; ++v) {
        const Vec3d n{D(mesh.normals[Idx(v * 3)]), D(mesh.normals[Idx(v * 3 + 1)]),
                      D(mesh.normals[Idx(v * 3 + 2)])};
        const Vec3d p{D(mesh.positions[Idx(v * 3)]), D(mesh.positions[Idx(v * 3 + 1)]),
                      D(mesh.positions[Idx(v * 3 + 2)])};
        worst_length = std::max(worst_length, std::fabs(n.Length() - 1.0));
        worst_outward = std::min(worst_outward, n.Dot((p - Vec3d{0.5, 0.5, 0.5}).Normalized()));
    }
    std::printf("  normals: unit to %.2e, worst outward agreement %.6f\n", worst_length,
                worst_outward);
    CHECK(worst_length < 1e-6);
    CHECK(worst_outward > 0.0);

    // The auto scale makes the largest displacement a fixed fraction of
    // the model's diagonal, whatever the units. That is the property, so
    // that is what is measured -- not the scale factor itself, which
    // means nothing on its own.
    RenderOptions deformed;
    deformed.auto_scale_fraction = 0.08;
    RenderMesh moved;
    CHECK(fem::RenderSurface(model, displacement, field, deformed, &moved, &error));
    double largest = 0.0;
    for (int v = 0; v < moved.vertex_count; ++v) {
        const Vec3d p{D(moved.positions[Idx(v * 3)]), D(moved.positions[Idx(v * 3 + 1)]),
                      D(moved.positions[Idx(v * 3 + 2)])};
        const Vec3d original{D(mesh.positions[Idx(v * 3)]), D(mesh.positions[Idx(v * 3 + 1)]),
                             D(mesh.positions[Idx(v * 3 + 2)])};
        largest = std::max(largest, (p - original).Length());
    }
    const double diagonal = std::sqrt(3.0);
    std::printf("  auto scale %.4e puts the largest movement at %.6f of the diagonal\n",
                moved.scale_used, largest / diagonal);
    // A float's worth of precision, because the positions a renderer is
    // handed are floats. The self-check numbers are not.
    CHECK(Near(largest / diagonal, 0.08, 1e-6));
    // And it is still the same surface: exaggerating a displacement must
    // not change the topology.
    CHECK(moved.triangle_count == mesh.triangle_count);
    CHECK(Watertight(moved, &unmatched));

    // Clipping keeps the half the caller asked for and drops the rest.
    RenderOptions clipped;
    clipped.undeformed = true;
    clipped.clip = true;
    clipped.clip_point = Vec3d{0.5, 0.5, 0.5};
    clipped.clip_normal = Vec3d{1, 0, 0};
    RenderMesh half;
    CHECK(fem::RenderSurface(model, displacement, field, clipped, &half, &error));
    std::printf("  clipped at x = 0.5: %d of %d triangles, area %.12f\n", half.triangle_count,
                mesh.triangle_count, half.area);
    CHECK(half.triangle_count < mesh.triangle_count);
    // Exactly half the cube's surface lies at x >= 0.5. Keeping every
    // triangle with a vertex across the plane instead of only those
    // wholly across gave four, which is that three plus a ring of the
    // discarded half one element deep -- the kind of error that looks
    // like a rendering artefact and is arithmetic.
    CHECK(Near(half.area, 3.0, 1e-12));
}

// --- Sections and isosurfaces ---------------------------------------------

void TestSection() {
    std::printf("plane sections, against their exact cross-sections:\n");
    AnalysisModel model = Cube(ElementShape::Hex8, 6);
    const std::vector<Vec3d> still = NoDisplacement(model);
    std::vector<double> field(model.nodes.size(), 1.0);
    RenderOptions options;
    options.undeformed = true;

    struct Case {
        const char *what;
        Vec3d point;
        Vec3d normal;
        double area;
    };
    // The diagonal cut is the one worth having: it crosses every element
    // at a different angle, so a marching case that was got wrong shows
    // up as an area that is wrong rather than as a picture that is
    // slightly odd. A plane through the centre normal to (1,1,0) cuts the
    // unit cube in a rectangle 1 by sqrt(2).
    const Case cases[] = {
        {"normal to x, through the middle", Vec3d{0.5, 0.5, 0.5}, Vec3d{1, 0, 0}, 1.0},
        {"normal to (1,1,0), through the middle", Vec3d{0.5, 0.5, 0.5}, Vec3d{1, 1, 0},
         std::sqrt(2.0)},
        {"normal to (1,1,1), through a corner", Vec3d{1.0 / 3.0, 1.0 / 3.0, 1.0 / 3.0},
         Vec3d{1, 1, 1}, std::sqrt(3.0) / 2.0},
    };
    for (const Case &one : cases) {
        RenderMesh mesh;
        std::string error;
        CHECK(fem::RenderSection(model, still, field, one.point, one.normal, options, &mesh,
                                 &error));
        std::printf("  %-38s %4d triangles, area %.12f (exact %.12f)\n", one.what,
                    mesh.triangle_count, mesh.area, one.area);
        CHECK(Near(mesh.area, one.area, 1e-12));
    }
    // A plane that misses the model entirely produces nothing, rather
    // than a failure or an empty-but-malformed mesh.
    RenderMesh nothing;
    std::string error;
    CHECK(fem::RenderSection(model, still, field, Vec3d{5, 0, 0}, Vec3d{1, 0, 0}, options, &nothing,
                             &error));
    CHECK(nothing.triangle_count == 0);
    CHECK(nothing.vertex_count == 0);
    CHECK(!fem::RenderSection(model, still, field, Vec3d{0, 0, 0}, Vec3d{0, 0, 0}, options,
                              &nothing, &error));
}

void TestIsosurface() {
    std::printf("isosurfaces, against a sphere of known area:\n");
    // THE FIELD IS A RADIUS AND THE ANSWER IS A SPHERE, so the area is
    // 4 pi r^2 exactly and the error must fall as the mesh is refined.
    // A number that is merely close proves nothing -- a bug that drops
    // one marching case in eight is also close -- so the rate is what is
    // checked.
    const double radius = 0.3;
    const Vec3d centre{0.5, 0.5, 0.5};
    double error_at[3] = {0, 0, 0};
    double volume_error_at[3] = {0, 0, 0};
    const int meshes[3] = {8, 16, 32};
    for (int i = 0; i < 3; ++i) {
        AnalysisModel model = Cube(ElementShape::Hex8, meshes[i]);
        const std::vector<Vec3d> still = NoDisplacement(model);
        std::vector<double> distance(model.nodes.size());
        std::vector<double> height(model.nodes.size());
        for (int node = 0; node < model.NodeCount(); ++node) {
            distance[Idx(node)] = (model.nodes[Idx(node)] - centre).Length();
            height[Idx(node)] = model.nodes[Idx(node)].z;
        }
        RenderOptions options;
        options.undeformed = true;
        RenderMesh mesh;
        std::string message;
        CHECK(fem::RenderIsosurface(model, still, distance, radius, height, options, &mesh,
                                    &message));
        const double exact_area = 4.0 * cad::kPi * radius * radius;
        const double exact_volume = 4.0 / 3.0 * cad::kPi * radius * radius * radius;
        error_at[i] = std::fabs(mesh.area - exact_area) / exact_area;
        volume_error_at[i] = std::fabs(mesh.enclosed_volume - exact_volume) / exact_volume;
        int unmatched = 0;
        const bool closed = WatertightByPosition(mesh, 1e-9, &unmatched);
        std::printf("  %2d per side: %5d triangles, area %.6f vs %.6f (%.3f%%),"
                    " volume %.6f vs %.6f\n",
                    meshes[i], mesh.triangle_count, mesh.area, exact_area, 100.0 * error_at[i],
                    mesh.enclosed_volume, exact_volume);
        CHECK(closed);
        if (!closed) std::printf("    %d unmatched edges\n", unmatched);
        // BOTH FROM BELOW, ALWAYS. The triangulated surface has its
        // vertices on the sphere and its faces inside it, so an
        // inscribed polyhedron cannot have more area or enclose more
        // volume than the sphere does. A number that overshot would mean
        // a triangle counted twice or wound the wrong way, and the sign
        // catches that where a magnitude would not.
        CHECK(mesh.area < exact_area);
        CHECK(mesh.enclosed_volume < exact_volume);
        // Coloured by height, not by the field being contoured, which is
        // the point of an isosurface plot: the range must be the range of
        // the colouring field over the sphere, not over the cube.
        CHECK(mesh.field_min >= 0.0 && mesh.field_min < centre.z - radius + 0.05);
        CHECK(mesh.field_max <= 1.0 && mesh.field_max > centre.z + radius - 0.05);
    }
    const double rate = 0.5 * (std::log2(error_at[0] / error_at[1]) +
                               std::log2(error_at[1] / error_at[2]));
    const double volume_rate = 0.5 * (std::log2(volume_error_at[0] / volume_error_at[1]) +
                                      std::log2(volume_error_at[1] / volume_error_at[2]));
    std::printf("  the area error falls at rate %.3f, the volume error at %.3f\n", rate,
                volume_rate);
    // A RATE RATHER THAN A TOLERANCE, because a bug that drops one
    // marching case in eight also lands within a few percent on a coarse
    // mesh and stays there. Second order is what a piecewise-linear
    // surface through the exact level set gives, and it is what both
    // measures have to show.
    CHECK(rate > 1.5);
    CHECK(volume_rate > 1.5);
    CHECK(volume_error_at[2] < 0.01);

    // A level nothing reaches gives an empty surface, not a failure.
    AnalysisModel model = Cube(ElementShape::Hex8, 4);
    std::vector<double> field(model.nodes.size(), 1.0);
    RenderMesh empty;
    std::string message;
    RenderOptions options;
    options.undeformed = true;
    CHECK(fem::RenderIsosurface(model, NoDisplacement(model), field, 5.0, {}, options, &empty,
                                &message));
    CHECK(empty.triangle_count == 0);
}

void TestIsosurfaceOnTets() {
    std::printf("the same isosurface on a tetrahedral mesh:\n");
    // Different decomposition, different marching cases exercised, same
    // sphere. If the two disagree, one of the decompositions is wrong,
    // and neither would show it alone.
    const double radius = 0.3;
    const Vec3d centre{0.5, 0.5, 0.5};
    for (const ElementShape shape : {ElementShape::Tet4, ElementShape::Tet10}) {
        AnalysisModel model = Cube(shape, 12);
        std::vector<double> distance(model.nodes.size());
        for (int node = 0; node < model.NodeCount(); ++node) {
            distance[Idx(node)] = (model.nodes[Idx(node)] - centre).Length();
        }
        RenderOptions options;
        options.undeformed = true;
        RenderMesh mesh;
        std::string message;
        CHECK(fem::RenderIsosurface(model, NoDisplacement(model), distance, radius, {}, options,
                                    &mesh, &message));
        const double exact = 4.0 * cad::kPi * radius * radius;
        int unmatched = 0;
        const bool closed = WatertightByPosition(mesh, 1e-9, &unmatched);
        std::printf("  %-6s %5d triangles, area %.6f vs %.6f (%.3f%%), watertight %s\n",
                    fem::ElementShapeName(shape), mesh.triangle_count, mesh.area, exact,
                    100.0 * std::fabs(mesh.area - exact) / exact, closed ? "yes" : "no");
        CHECK(closed);
        CHECK(std::fabs(mesh.area - exact) < exact * 0.03);
    }
}

// --- Vectors ---------------------------------------------------------------

void TestVectors() {
    std::printf("vector glyphs:\n");
    AnalysisModel model = Cube(ElementShape::Hex8, 2);
    std::vector<Vec3d> vectors(model.nodes.size());
    for (int node = 0; node < model.NodeCount(); ++node) {
        vectors[Idx(node)] = Vec3d{0, 0, -1e-6 * (1.0 + model.nodes[Idx(node)].x)};
    }
    fem::VectorOptions options;
    RenderMesh mesh;
    std::string error;
    CHECK(fem::RenderVectors(model, model.nodes, vectors, options, &mesh, &error));
    // Three segments per arrow -- a shaft and two barbs -- and no
    // triangles at all, because an arrow drawn as a solid is a cone and
    // this is deliberately not one.
    const int arrows = model.NodeCount();
    std::printf("  %d arrows: %d line segments, %d triangles, magnitudes %.3e to %.3e\n", arrows,
                static_cast<int>(mesh.line_indices.size() / 2), mesh.triangle_count,
                mesh.field_min, mesh.field_max);
    CHECK(static_cast<int>(mesh.line_indices.size()) == arrows * 6);
    CHECK(mesh.triangle_count == 0);
    CHECK(Near(mesh.field_min, 1e-6, 1e-18));
    CHECK(Near(mesh.field_max, 2e-6, 1e-18));

    // The auto scale puts the longest arrow at a fixed fraction of the
    // diagonal, which is the same property the deformed shape has and for
    // the same reason.
    double longest = 0.0;
    for (std::size_t i = 0; i + 1 < mesh.line_indices.size(); i += 6) {
        const unsigned int a = mesh.line_indices[i], b = mesh.line_indices[i + 1];
        const Vec3d tail{D(mesh.positions[a * 3]), D(mesh.positions[a * 3 + 1]),
                         D(mesh.positions[a * 3 + 2])};
        const Vec3d tip{D(mesh.positions[b * 3]), D(mesh.positions[b * 3 + 1]),
                        D(mesh.positions[b * 3 + 2])};
        longest = std::max(longest, (tip - tail).Length());
    }
    std::printf("  longest arrow %.6f of the diagonal\n", longest / std::sqrt(3.0));
    CHECK(Near(longest / std::sqrt(3.0), options.auto_scale_fraction, 1e-6));

    // Striding thins the plot out, which is the only way a vector plot of
    // a real mesh is readable at all.
    fem::VectorOptions thinned = options;
    thinned.stride = 4;
    RenderMesh fewer;
    CHECK(fem::RenderVectors(model, model.nodes, vectors, thinned, &fewer, &error));
    std::printf("  every 4th node: %d segments instead of %d\n",
                static_cast<int>(fewer.line_indices.size() / 2),
                static_cast<int>(mesh.line_indices.size() / 2));
    CHECK(fewer.line_indices.size() * 4 <= mesh.line_indices.size() + 24);
    CHECK(!fewer.line_indices.empty());

    // A zero vector draws nothing rather than a degenerate glyph.
    std::vector<Vec3d> none(model.nodes.size(), Vec3d{});
    RenderMesh blank;
    CHECK(fem::RenderVectors(model, model.nodes, none, options, &blank, &error));
    CHECK(blank.line_indices.empty());
    CHECK(!fem::RenderVectors(model, model.nodes, std::vector<Vec3d>(3), options, &blank, &error));
}

void TestColoursComeFromOnePlace() {
    std::printf("colours agree with the field they claim to show:\n");
    // A CONTOUR AND A PROBE MUST AGREE. They do here because there is one
    // function, and this checks the rest of the path: that a vertex whose
    // field value is at the top of the range gets the top of the colour
    // map, and one at the bottom gets the bottom, with nothing
    // transposed in between.
    AnalysisModel model = Cube(ElementShape::Hex8, 3);
    std::vector<double> field(model.nodes.size());
    for (int node = 0; node < model.NodeCount(); ++node) field[Idx(node)] = model.nodes[Idx(node)].x;
    RenderOptions options;
    options.undeformed = true;
    RenderMesh mesh;
    std::string error;
    CHECK(fem::RenderSurface(model, NoDisplacement(model), field, options, &mesh, &error));
    unsigned char lowest[4];
    unsigned char highest[4];
    fem::MapColor(options.color_map, 0.0, lowest);
    fem::MapColor(options.color_map, 1.0, highest);
    int at_low = 0;
    int at_high = 0;
    for (int v = 0; v < mesh.vertex_count; ++v) {
        const double x = D((mesh.positions[Idx(v * 3)]));
        const unsigned char *rgba = &mesh.colors[Idx(v * 4)];
        if (x <= 1e-12) {
            ++at_low;
            for (int c = 0; c < 4; ++c) CHECK(rgba[c] == lowest[c]);
        }
        if (x >= 1.0 - 1e-12) {
            ++at_high;
            for (int c = 0; c < 4; ++c) CHECK(rgba[c] == highest[c]);
        }
    }
    std::printf("  %d vertices at the bottom of the range and %d at the top, all exact\n", at_low,
                at_high);
    CHECK(at_low == 16 && at_high == 16);

    // A FIXED RANGE IS WHAT MAKES TWO LOAD CASES COMPARABLE, and
    // auto-ranging actively prevents it: double the field and the picture
    // is identical under auto, and visibly different under a fixed range.
    std::vector<double> doubled = field;
    for (double &value : doubled) value *= 2.0;
    RenderMesh scaled;
    CHECK(fem::RenderSurface(model, NoDisplacement(model), doubled, options, &scaled, &error));
    CHECK(scaled.colors == mesh.colors);
    RenderOptions fixed = options;
    fixed.auto_range = false;
    fixed.range_min = 0.0;
    fixed.range_max = 2.0;
    RenderMesh one;
    RenderMesh two;
    CHECK(fem::RenderSurface(model, NoDisplacement(model), field, fixed, &one, &error));
    CHECK(fem::RenderSurface(model, NoDisplacement(model), doubled, fixed, &two, &error));
    std::printf("  under auto-ranging a doubled field draws identically; under a fixed range"
                " it does not\n");
    CHECK(one.colors != two.colors);

    // A constant field must not divide by zero: it draws the middle of
    // the map, at every vertex.
    std::vector<double> flat(model.nodes.size(), 7.0);
    RenderMesh even;
    CHECK(fem::RenderSurface(model, NoDisplacement(model), flat, options, &even, &error));
    unsigned char middle[4];
    fem::MapColor(options.color_map, 0.5, middle);
    bool all_middle = true;
    for (int v = 0; v < even.vertex_count; ++v) {
        for (int c = 0; c < 4; ++c) {
            if (even.colors[Idx(v * 4 + c)] != middle[c]) all_middle = false;
        }
    }
    CHECK(all_middle);

    // Wrong-sized inputs are refused rather than read past.
    RenderMesh ignored;
    CHECK(!fem::RenderSurface(model, NoDisplacement(model), std::vector<double>(3), options,
                              &ignored, &error));
}

}  // namespace

int main() {
    TestDecomposition();
    TestSurface();
    TestSection();
    TestIsosurface();
    TestIsosurfaceOnTets();
    TestVectors();
    TestColoursComeFromOnePlace();
    if (failures == 0) {
        std::printf("fem_render_test passed (%d checks)\n", checks);
        return 0;
    }
    std::printf("fem_render_test FAILED (%d of %d checks)\n", failures, checks);
    return 1;
}
