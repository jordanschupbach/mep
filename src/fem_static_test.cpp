// Part H.6: linear static analysis, verified.

#include "fem_static.h"

#include "cad_boolean.h"
#include "cad_feature.h"
#include "cad_pcurve.h"

#include <algorithm>
#include <map>
#include <utility>
#include <cmath>
#include <cstdio>
#include <string>
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
#define CHECK_MESSAGE(x, message) Check((x), (std::string(#x) + ": " + (message)).c_str(), __LINE__)

using cad::Vec3d;
using fem::AnalysisModel;
using fem::BoundElement;
using fem::ElementShape;
using fem::StaticOptions;
using fem::StaticResult;
using fem::StudyMaterial;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// --- The manufactured solution --------------------------------------------
//
// A DIVERGENCE-FREE FIELD, WHICH IS WHAT MAKES THE BODY FORCE WRITABLE.
// The Navier equation is (lambda + mu) grad(div u) + mu laplacian(u) + f
// = 0, and the first term is the awkward one: for a general field it
// needs second derivatives of every component crossed with every other.
// Choose u to be the curl of something and div u vanishes identically, so
// f = -mu laplacian(u) and nothing else. Taking the curl of (0, 0, phi)
// with phi = sin(pi x) sin(pi y) sin(pi z) gives a field whose Laplacian
// is -3 pi^2 times itself, so the body force is 3 pi^2 mu u -- one line,
// with no chance of the "exact" solution being exact for a different
// problem than the one solved.
constexpr double kPi = 3.14159265358979323846;

Vec3d Exact(const Vec3d &p) {
    const double sx = std::sin(kPi * p.x);
    const double sy = std::sin(kPi * p.y);
    const double sz = std::sin(kPi * p.z);
    const double cx = std::cos(kPi * p.x);
    const double cy = std::cos(kPi * p.y);
    return Vec3d{kPi * sx * cy * sz, -kPi * cx * sy * sz, 0.0};
}

double ShearModulus(const StudyMaterial &material) {
    const double e = material.youngs_modulus.At(20.0);
    const double v = material.poissons_ratio.At(20.0);
    return e / (2.0 * (1.0 + v));
}

Vec3d BodyForce(const Vec3d &p, double mu) { return Exact(p) * (3.0 * kPi * kPi * mu); }

// --- Meshes of the unit cube, one per element type -------------------------

AnalysisModel Cube(ElementShape shape, int m) {
    AnalysisModel model;
    StudyMaterial material;
    material.youngs_modulus = fem::MaterialCurve::Constant(1.0);
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
                // Six tetrahedra about the 0-6 diagonal: tiles without a
                // gap and matches face for face between cubes.
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

    // Mid-edge nodes, added once per edge and shared, which is what makes
    // the mesh conforming.
    static const int edges[6][2] = {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}};
    std::map<std::pair<int, int>, int> middles;
    for (BoundElement &element : model.elements) {
        element.shape = ElementShape::Tet10;
        const std::vector<int> corners = element.nodes;
        for (const auto &edge : edges) {
            const int u = corners[Idx(edge[0])];
            const int v = corners[Idx(edge[1])];
            const std::pair<int, int> key = u < v ? std::make_pair(u, v) : std::make_pair(v, u);
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

// The consistent nodal load of the body force: the integral of the shape
// functions against it over each element.
//
// EXACTLY THE SAME MACHINERY THE LOADS USE, which is the point: if the
// integration were wrong the manufactured solution would converge to
// something that is not the exact field, and the convergence *rate* would
// still look right. That is why the rate is checked against an analytic
// field and not against a finer mesh of itself.
void ApplyBodyForce(AnalysisModel *model, double mu) {
    std::vector<double> load(Idx(model->NodeCount() * 3), 0.0);
    std::vector<fem::QuadraturePoint> rule;
    std::vector<double> n;
    std::vector<double> dn;
    std::vector<double> dn_xyz;
    std::vector<Vec3d> corner;
    std::string error;
    for (const BoundElement &element : model->elements) {
        // Two degrees above the element's own, since the body force is a
        // sine rather than a polynomial and under-integrating it would
        // look exactly like an element that converges slowly.
        if (!fem::Quadrature(element.shape, 4, &rule, &error)) continue;
        corner.clear();
        for (const int node : element.nodes) corner.push_back(model->nodes[Idx(node)]);
        for (const fem::QuadraturePoint &point : rule) {
            fem::ShapeFunctions(element.shape, point.at, &n, &dn);
            const double determinant = fem::ElementJacobian(element.shape, corner, dn, &dn_xyz);
            Vec3d where{};
            for (std::size_t a = 0; a < element.nodes.size(); ++a) {
                where = where + corner[a] * n[a];
            }
            const Vec3d force = BodyForce(where, mu) * (determinant * point.weight);
            for (std::size_t a = 0; a < element.nodes.size(); ++a) {
                load[Idx(element.nodes[a] * 3 + 0)] += force.x * n[a];
                load[Idx(element.nodes[a] * 3 + 1)] += force.y * n[a];
                load[Idx(element.nodes[a] * 3 + 2)] += force.z * n[a];
            }
        }
    }
    for (int node = 0; node < model->NodeCount(); ++node) {
        const Vec3d force{load[Idx(node * 3 + 0)], load[Idx(node * 3 + 1)],
                          load[Idx(node * 3 + 2)]};
        if (force.LengthSquared() == 0.0) continue;
        model->loads.push_back(fem::NodalLoad{node, force});
    }
}

// The exact field imposed on every boundary node. The manufactured field
// vanishes on the cube's faces in two of its components and not the
// third, so this is not the same as fixing the boundary.
void ConstrainBoundary(AnalysisModel *model) {
    for (int node = 0; node < model->NodeCount(); ++node) {
        const Vec3d &p = model->nodes[Idx(node)];
        const bool boundary = p.x == 0.0 || p.x == 1.0 || p.y == 0.0 || p.y == 1.0 || p.z == 0.0 ||
                              p.z == 1.0;
        if (!boundary) continue;
        const Vec3d value = Exact(p);
        fem::Constraint constraint;
        constraint.node = node;
        for (int axis = 0; axis < 3; ++axis) constraint.fixed[axis] = true;
        constraint.value[0] = value.x;
        constraint.value[1] = value.y;
        constraint.value[2] = value.z;
        model->constraints.push_back(constraint);
    }
}

// The L2 norm of the error, integrated over the elements rather than
// sampled at the nodes. Sampling at nodes measures the one place a
// Galerkin method is most accurate and would flatter every element in the
// library.
double ErrorNorm(const AnalysisModel &model, const StaticResult &result) {
    double squared = 0.0;
    std::vector<fem::QuadraturePoint> rule;
    std::vector<double> n;
    std::vector<double> dn;
    std::vector<double> dn_xyz;
    std::vector<Vec3d> corner;
    std::string error;
    for (const BoundElement &element : model.elements) {
        if (!fem::Quadrature(element.shape, 4, &rule, &error)) continue;
        corner.clear();
        for (const int node : element.nodes) corner.push_back(model.nodes[Idx(node)]);
        for (const fem::QuadraturePoint &point : rule) {
            fem::ShapeFunctions(element.shape, point.at, &n, &dn);
            const double determinant = fem::ElementJacobian(element.shape, corner, dn, &dn_xyz);
            Vec3d where{};
            Vec3d computed{};
            for (std::size_t a = 0; a < element.nodes.size(); ++a) {
                where = where + corner[a] * n[a];
                computed = computed + result.displacement[Idx(element.nodes[a])] * n[a];
            }
            const Vec3d difference = computed - Exact(where);
            squared += difference.LengthSquared() * determinant * point.weight;
        }
    }
    return std::sqrt(squared);
}

void TestManufacturedSolution() {
    std::printf("a manufactured solution, and the rate it converges at:\n");
    struct Case {
        ElementShape shape;
        const char *name;
        double expected_order;
        std::vector<int> refinements;
    };
    const std::vector<Case> cases = {
        {ElementShape::Hex8, "Hex8", 2.0, {2, 4, 8, 12}},
        {ElementShape::Tet4, "Tet4", 2.0, {2, 4, 8, 12}},
        {ElementShape::Tet10, "Tet10", 3.0, {1, 2, 4, 6}},
    };
    for (const Case &which : cases) {
        std::vector<double> sizes;
        std::vector<double> errors;
        for (const int m : which.refinements) {
            AnalysisModel model = Cube(which.shape, m);
            const double mu = ShearModulus(model.materials[0]);
            ApplyBodyForce(&model, mu);
            ConstrainBoundary(&model);
            StaticResult result;
            StaticOptions options;
            CHECK_MESSAGE(fem::SolveStatic(model, {}, options, &result), result.error);
            if (!result.ok) return;
            // Equilibrium, every time: the reactions balance the loads, or
            // the solve did not solve what it was given.
            CHECK(result.equilibrium_residual < 1e-10);
            sizes.push_back(1.0 / static_cast<double>(m));
            errors.push_back(ErrorNorm(model, result));
        }
        std::printf("  %-6s expecting order %.0f\n", which.name, which.expected_order);
        std::vector<double> orders;
        for (std::size_t i = 0; i < sizes.size(); ++i) {
            if (i == 0) {
                std::printf("      h = %.4f   error %.4e\n", sizes[i], errors[i]);
                continue;
            }
            const double order = std::log(errors[i - 1] / errors[i]) / std::log(sizes[i - 1] / sizes[i]);
            orders.push_back(order);
            std::printf("      h = %.4f   error %.4e   order %.3f\n", sizes[i], errors[i], order);
        }
        // THE RATE IS THE TEST. An element that is subtly wrong still
        // converges -- to the wrong answer, or at the wrong rate -- and
        // neither shows in a single mesh's error.
        //
        // JUDGED ON THE FINEST PAIR, AND ON THE TREND. The rate is
        // asymptotic: on the coarsest mesh here a linear tetrahedron
        // manages 1.29 against an expected 2, and that is the mesh being
        // coarse rather than the element being wrong. So the test asks
        // that the finest pair reaches the order and that each refinement
        // does better than the last, which together say the sequence is
        // heading where it should -- and a wrong element fails both.
        CHECK(orders.back() > which.expected_order - 0.25);
        CHECK(orders.back() < which.expected_order + 0.6);
        for (std::size_t i = 1; i < orders.size(); ++i) {
            CHECK(orders[i] > orders[i - 1] - 0.15);
        }
        // And the error really does fall: a rate computed from two large
        // errors can look right while nothing is converging.
        CHECK(errors.back() < errors.front() * 0.2);
    }
}

// --- A closed-form answer on curved geometry ------------------------------
//
// A SOLID CYLINDER UNDER UNIFORM EXTERNAL PRESSURE, held against axial
// motion at both ends so that plane strain applies. The closed form is as
// simple as elasticity gets and as sharp a test as there is: the stress
// state is *uniform*, sigma_r = sigma_theta = -p everywhere, so the patch
// test guarantees that a correct element reproduces it exactly. Anything
// that is not exact here is the faceting of the boundary the pressure was
// applied to, and nothing else.
//
//     sigma_r = sigma_theta = -p,  sigma_z = -2 v p
//     u_r(r) = -r p (1 + v)(1 - 2v) / E
//
// Worth having next to the manufactured solution because it tests what
// the manufactured solution does not: a mesh from the mesher rather than
// from a nested loop, a pressure applied through Part H.2's surface
// integral over curved facets, and stress recovered against a value that
// is known rather than fitted.
//
// A THICK-WALLED CYLINDER WOULD HAVE BEEN BETTER AND DOES NOT WORK YET.
// Lame's annulus varies through the wall, so it would test the stress
// gradient and not just its level. Two things stopped it, both outside
// this part and both recorded rather than worked around:
//   * A full annulus has annular flat ends -- planar faces with an inner
//     loop -- and Part G.2's surface mesher does not mesh a face with a
//     hole in it. It produced a surface that was not closed.
//   * Taking a quarter to remove the hole needs an annulus intersected
//     with a box, and Part C.4's boolean returns an open shell for that,
//     reporting seven edges used by one coedge each.
void TestPressurisedCylinder() {
    std::printf("a cylinder under uniform external pressure:\n");
    // METRES, NOT MILLIMETRES, AND THAT IS A FINDING RATHER THAN A
    // CHOICE. A 50 mm cylinder -- an ordinary size for a part -- meshes
    // into slivers: the same shape at radius 0.5, 2 and 5 gives a worst
    // dihedral angle of 15 degrees and no slivers at all, and at radius
    // 0.05 gives 0.05 degrees and twenty-five of them, with the same
    // element and node counts. Part G's own cylinder test shrunk forty
    // times goes from no slivers to a hundred and twenty. Something in
    // the surface mesher is an absolute length where it should be a
    // relative one; the sizing field is not it, since the node counts are
    // identical at every scale. It is recorded in Part G rather than
    // worked around here, and the physics below is scale-free, so
    // verifying it at a size that meshes verifies it.
    const double radius = 2.0;
    const double height = 0.8;
    const double pressure = 10e6;
    const double youngs = 210e9;
    const double poisson = 0.3;

    cad::Model model;
    cad::EntityId body = cad::kNoEntity;
    std::string error;
    CHECK(cad::MakeCylinder(Vec3d{0, 0, 0}, Vec3d{0, 0, 1}, radius, height, &model, &body));
    CHECK(cad::BuildAllPCurves(&model, {}, &error));

    fem::VolumeMeshOptions mesh_options;
    mesh_options.surface.sizing.target = 0.32;
    fem::VolumeMesh mesh;
    fem::MeshReport report;
    CHECK_MESSAGE(fem::MeshBody(model, {body}, mesh_options, &mesh, &report), report.error);
    if (!report.ok) return;

    // The curved face: the cylindrical one.
    cad::EntityId side = cad::kNoEntity;
    for (const cad::Face &face : model.Faces()) {
        const cad::Surface *surface = model.SurfaceAt(face.surface);
        if (surface == nullptr || surface->Kind() != cad::SurfaceKind::Cylinder) continue;
        if (!fem::NodesOnFace(mesh, face.id).empty()) side = face.id;
    }
    CHECK(side != cad::kNoEntity);
    if (side == cad::kNoEntity) return;

    AnalysisModel analysis;
    StudyMaterial steel;
    steel.youngs_modulus = fem::MaterialCurve::Constant(youngs);
    steel.poissons_ratio = fem::MaterialCurve::Constant(poisson);
    analysis.materials.push_back(steel);
    analysis.nodes = mesh.nodes;
    for (std::size_t i = 0; i < mesh.tets.size(); ++i) {
        fem::BoundElement element;
        element.shape = ElementShape::Tet4;
        element.nodes = {mesh.tets[i].a, mesh.tets[i].b, mesh.tets[i].c, mesh.tets[i].d};
        analysis.elements.push_back(element);
    }
    // Plane strain from holding both flat ends axially, and the remaining
    // rigid motions removed by the two symmetry planes -- which the exact
    // solution satisfies, so they constrain nothing real.
    for (int node = 0; node < analysis.NodeCount(); ++node) {
        const Vec3d &p = analysis.nodes[Idx(node)];
        fem::Constraint constraint;
        constraint.node = node;
        bool any = false;
        if (std::fabs(p.z) < 1e-9 || std::fabs(p.z - height) < 1e-9) {
            constraint.fixed[2] = true;
            any = true;
        }
        if (std::fabs(p.x) < 1e-9) {
            constraint.fixed[0] = true;
            any = true;
        }
        if (std::fabs(p.y) < 1e-9) {
            constraint.fixed[1] = true;
            any = true;
        }
        if (any) analysis.constraints.push_back(constraint);
    }
    std::vector<fem::NodalLoad> applied;
    double area = 0.0;
    CHECK_MESSAGE(fem::FaceTraction(mesh, side, Vec3d{}, pressure, &applied, &area, &error), error);
    analysis.loads = applied;
    const double exact_area = 2.0 * kPi * radius * height;
    std::printf("  %d elements, curved area %.6f m2 (a true cylinder has %.6f, %+.2f%%)\n",
                analysis.ElementCount(), area, exact_area, (area / exact_area - 1.0) * 100.0);
    // The faceted area is smaller than the cylinder's, which is the whole
    // source of the error below.
    CHECK(area < exact_area);
    CHECK(area > exact_area * 0.98);

    StaticResult result;
    StaticOptions options;
    CHECK_MESSAGE(fem::SolveStatic(analysis, {}, options, &result), result.error);
    if (!result.ok) return;
    CHECK(result.equilibrium_residual < 1e-9);

    const double exact_radial = -radius * pressure * (1.0 + poisson) * (1.0 - 2.0 * poisson) / youngs;
    double at_rim = 0.0;
    int rim_nodes = 0;
    for (int node = 0; node < analysis.NodeCount(); ++node) {
        const Vec3d &p = analysis.nodes[Idx(node)];
        const double r = std::sqrt(p.x * p.x + p.y * p.y);
        if (std::fabs(r - radius) > 1e-6) continue;
        at_rim += (result.displacement[Idx(node)].x * p.x + result.displacement[Idx(node)].y * p.y) / r;
        ++rim_nodes;
    }
    CHECK(rim_nodes > 0);
    at_rim /= std::max(1, rim_nodes);
    std::printf("  radial displacement at the rim %.6e, exactly %.6e (%+.2f%%)\n", at_rim,
                exact_radial, (at_rim / exact_radial - 1.0) * 100.0);
    CHECK(at_rim < 0.0);
    CHECK(std::fabs(at_rim - exact_radial) < std::fabs(exact_radial) * 0.03);

    // THE STRESS IS UNIFORM, AND THAT IS THE SHARP PART. A constant-stress
    // state is in every element's own space, so the patch test says a
    // correct element reproduces it exactly; the spread across the mesh is
    // therefore a direct measure of how much the faceted boundary differs
    // from the cylinder, and nothing else can hide in it.
    double lowest = 1e30;
    double highest = -1e30;
    double mean = 0.0;
    for (int e = 0; e < analysis.ElementCount(); ++e) {
        const fem::StressTensor &s = result.element_stress[Idx(e)];
        const double in_plane = 0.5 * (s.s[0] + s.s[1]);
        lowest = std::min(lowest, in_plane);
        highest = std::max(highest, in_plane);
        mean += in_plane;
    }
    mean /= static_cast<double>(analysis.ElementCount());
    std::printf("  in-plane stress %.4e on average, %.4e to %.4e across the mesh (exactly %.4e)\n",
                mean, lowest, highest, -pressure);
    CHECK(std::fabs(mean + pressure) < pressure * 0.05);
    // Axial stress follows from plane strain and is a second, independent
    // reading of the same solution.
    double axial = 0.0;
    for (int e = 0; e < analysis.ElementCount(); ++e) {
        axial += result.element_stress[Idx(e)].s[2];
    }
    axial /= static_cast<double>(analysis.ElementCount());
    std::printf("  axial stress %.4e on average (plane strain gives %.4e)\n", axial,
                -2.0 * poisson * pressure);
    CHECK(std::fabs(axial + 2.0 * poisson * pressure) < 2.0 * poisson * pressure * 0.10);
}

}  // namespace

int main() {
    TestManufacturedSolution();
    TestPressurisedCylinder();
    if (failures != 0) {
        std::printf("fem_static_test FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    std::printf("fem_static_test passed (%d checks)\n", checks);
    return 0;
}
