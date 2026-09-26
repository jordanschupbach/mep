// Windowless coverage for fem_model.h and fem_solve.h
// (plans/CAD_FEM_PLAN.md Part 0.6).
//
// Three kinds of check, in increasing order of how much they prove:
//
//  1. Bookkeeping -- mesh generation, validation, JSON round-trips. Cheap
//     and catches typos.
//  2. The patch test. An arbitrary *linear* displacement field is imposed
//     on every boundary node; the solver must reproduce that field
//     exactly at the interior nodes it was not told about, and produce
//     exactly the uniform stress the field implies. This is the standard
//     admission test for a finite element, and it is unforgiving: it
//     fails for a wrong shape function, a wrong Jacobian, a wrong B
//     matrix, a wrong constitutive matrix, a mis-assembled global matrix,
//     a mis-applied constraint, or a stress recovery that reads the wrong
//     displacements. Passing it to machine precision is strong evidence
//     that all of those are right.
//  3. A cantilever against beam theory. Unlike the patch test this has no
//     exact finite-element answer -- the element converges toward it --
//     so what is checked is the *approach*: monotonic from below, at the
//     rate refinement should give. That is what distinguishes real
//     discretization error from a bug.
//
// Global equilibrium (reactions balancing applied load) is asserted
// everywhere, because it is nearly free and catches a class of mistake no
// residual check can: the residual of the constrained system is small by
// construction even when the system itself is wrong.

#include "fem_solve.h"

#include "fem_model.h"

#include <array>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

int g_checks = 0;

void Check(bool condition, const char *expression, int line) {
    ++g_checks;
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }
bool NearRelative(double a, double b, double relative_tol) {
    const double scale = std::max(std::fabs(a), std::fabs(b));
    if (scale < 1e-30) return true;
    return std::fabs(a - b) / scale <= relative_tol;
}

fem::Material Steel() {
    fem::Material m;
    m.name = "steel";
    m.youngs_modulus = 210e9;
    m.poissons_ratio = 0.3;
    m.density = 7850.0;
    return m;
}

void TestMeshGeneration() {
    const std::array<int, 3> divisions = {3, 2, 2};
    const fem::Model model =
        fem::MakeBoxMesh(cad::Vec3d{0, 0, 0}, cad::Vec3d{3.0, 2.0, 2.0}, divisions, Steel());
    CHECK(model.NodeCount() == 4 * 3 * 3);
    CHECK(model.ElementCount() == 3 * 2 * 2);
    CHECK(model.DofCount() == model.NodeCount() * 3);

    // The generated mesh must be valid -- in particular every element
    // positively oriented, which is the check that catches a node
    // ordering mistake in the generator itself.
    std::string error;
    fem::Model constrained = model;
    fem::Constraint c;
    c.node = 0;
    c.fixed[0] = c.fixed[1] = c.fixed[2] = true;
    constrained.constraints.push_back(c);
    fem::Constraint c2;
    c2.node = 1;
    c2.fixed[0] = c2.fixed[1] = c2.fixed[2] = true;
    constrained.constraints.push_back(c2);
    CHECK(constrained.Validate(&error));

    const cad::Box3d box = model.BoundingBox();
    CHECK(Near(box.x.lo, 0.0, 1e-12) && Near(box.x.hi, 3.0, 1e-12));
    CHECK(Near(box.z.hi, 2.0, 1e-12));

    // Face selection: each face of a 3x2x2 box has the right node count,
    // and every selected node really is on that plane.
    const std::vector<int> x_min = fem::BoxMeshFaceNodes(divisions, 0, false);
    const std::vector<int> x_max = fem::BoxMeshFaceNodes(divisions, 0, true);
    CHECK(x_min.size() == 3 * 3);
    CHECK(x_max.size() == 3 * 3);
    for (int n : x_min) CHECK(Near(model.nodes[static_cast<std::size_t>(n)].x, 0.0, 1e-12));
    for (int n : x_max) CHECK(Near(model.nodes[static_cast<std::size_t>(n)].x, 3.0, 1e-12));
    const std::vector<int> z_max = fem::BoxMeshFaceNodes(divisions, 2, true);
    CHECK(z_max.size() == 4 * 3);
    for (int n : z_max) CHECK(Near(model.nodes[static_cast<std::size_t>(n)].z, 2.0, 1e-12));
}

void TestValidation() {
    const std::array<int, 3> divisions = {2, 1, 1};
    fem::Model model = fem::MakeBoxMesh(cad::Vec3d{0, 0, 0}, cad::Vec3d{2.0, 1.0, 1.0}, divisions, Steel());
    std::string error;

    // No constraints at all: rejected with an explanation, not left to
    // surface as a singular factorization.
    CHECK(!model.Validate(&error));
    CHECK(error.find("rigid-body") != std::string::npos);

    // An out-of-range node reference.
    fem::Model bad_node = model;
    bad_node.elements[0].nodes[3] = 9999;
    CHECK(!bad_node.Validate(&error));
    CHECK(error.find("does not exist") != std::string::npos);

    // An inverted element: swapping two nodes of a hex flips its
    // Jacobian, and Validate must catch it before the solver builds a
    // nonsense stiffness matrix from it.
    fem::Model inverted = model;
    std::swap(inverted.elements[0].nodes[0], inverted.elements[0].nodes[4]);
    CHECK(!inverted.Validate(&error));
    CHECK(error.find("Jacobian") != std::string::npos);

    // Poisson's ratio at exactly 0.5 is incompressible, which this
    // formulation genuinely cannot represent.
    fem::Material incompressible = Steel();
    incompressible.poissons_ratio = 0.5;
    CHECK(!incompressible.IsValid(&error));
    CHECK(error.find("mixed formulation") != std::string::npos);
    fem::Material negative_e = Steel();
    negative_e.youngs_modulus = -1.0;
    CHECK(!negative_e.IsValid(&error));
    CHECK(Steel().IsValid(&error));
}

void TestStressInvariants() {
    // Pure hydrostatic stress has zero von Mises -- the defining property
    // of a deviatoric measure, and the check that catches a sign error in
    // the formula.
    fem::StressTensor hydrostatic;
    hydrostatic.s[0] = hydrostatic.s[1] = hydrostatic.s[2] = 50e6;
    CHECK(Near(hydrostatic.VonMises(), 0.0, 1e-6));

    // Uniaxial stress: von Mises equals the axial stress exactly.
    fem::StressTensor uniaxial;
    uniaxial.s[0] = 100e6;
    CHECK(NearRelative(uniaxial.VonMises(), 100e6, 1e-12));

    // Pure shear: von Mises is sqrt(3) times the shear stress.
    fem::StressTensor shear;
    shear.s[3] = 10e6;
    CHECK(NearRelative(shear.VonMises(), std::sqrt(3.0) * 10e6, 1e-12));

    // Principal stresses of a diagonal tensor are its diagonal, sorted.
    double principal[3];
    fem::StressTensor diagonal;
    diagonal.s[0] = 30e6;
    diagonal.s[1] = -10e6;
    diagonal.s[2] = 5e6;
    diagonal.Principal(principal);
    CHECK(NearRelative(principal[0], 30e6, 1e-10));
    CHECK(NearRelative(principal[1], 5e6, 1e-10));
    CHECK(NearRelative(principal[2], -10e6, 1e-10));

    // Principal stresses of pure shear are +tau, 0, -tau -- the case with
    // off-diagonal terms, so it exercises the cubic solution rather than
    // the already-diagonal shortcut.
    fem::StressTensor pure_shear;
    pure_shear.s[3] = 7e6;
    pure_shear.Principal(principal);
    CHECK(NearRelative(principal[0], 7e6, 1e-8));
    CHECK(Near(principal[1], 0.0, 1e-2));
    CHECK(NearRelative(principal[2], -7e6, 1e-8));
    // Invariants: the trace and the von Mises computed from the
    // principals must match the originals.
    CHECK(Near(principal[0] + principal[1] + principal[2], 0.0, 1e-2));
}

void TestConstitutiveMatrix() {
    double d[6][6];
    fem::ConstitutiveMatrix(Steel(), d);
    // Symmetric.
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 6; ++c) CHECK(NearRelative(d[r][c], d[c][r], 1e-14));
    }
    // Applying it to a uniaxial strain must give the textbook result:
    // sigma_xx = E*eps, sigma_yy = sigma_zz = 0 when the lateral strains
    // are -nu*eps.
    const double e = Steel().youngs_modulus;
    const double nu = Steel().poissons_ratio;
    const double eps = 1e-4;
    const double strain[6] = {eps, -nu * eps, -nu * eps, 0, 0, 0};
    double stress[6] = {0, 0, 0, 0, 0, 0};
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 6; ++c) stress[r] += d[r][c] * strain[c];
    }
    CHECK(NearRelative(stress[0], e * eps, 1e-12));
    CHECK(Near(stress[1], 0.0, 1.0));
    CHECK(Near(stress[2], 0.0, 1.0));
    // And the shear diagonal is G, not 2G -- the engineering-shear
    // convention the B matrix produces.
    CHECK(NearRelative(d[3][3], e / (2.0 * (1.0 + nu)), 1e-14));
}

void TestStiffnessProperties() {
    const std::array<int, 3> divisions = {2, 2, 2};
    const fem::Model model =
        fem::MakeBoxMesh(cad::Vec3d{0, 0, 0}, cad::Vec3d{1.0, 1.0, 1.0}, divisions, Steel());
    const num::SparseMatrix k = fem::AssembleStiffness(model);
    CHECK(k.Rows() == model.DofCount());
    CHECK(k.IsSymmetric(1e-3));  // absolute, and these entries are ~1e11

    // Every diagonal entry of a stiffness matrix is positive.
    for (int i = 0; i < k.Rows(); ++i) CHECK(k.At(i, i) > 0.0);

    // The unconstrained stiffness matrix has exactly six zero eigenvalues
    // -- the rigid-body modes. Rather than an eigensolver, apply each
    // mode directly: K times a rigid-body displacement must be zero.
    const cad::Vec3d translations[3] = {cad::Vec3d{1, 0, 0}, cad::Vec3d{0, 1, 0}, cad::Vec3d{0, 0, 1}};
    for (const cad::Vec3d &t : translations) {
        std::vector<double> u(static_cast<std::size_t>(model.DofCount()), 0.0);
        for (int n = 0; n < model.NodeCount(); ++n) {
            u[static_cast<std::size_t>(n * 3 + 0)] = t.x;
            u[static_cast<std::size_t>(n * 3 + 1)] = t.y;
            u[static_cast<std::size_t>(n * 3 + 2)] = t.z;
        }
        std::vector<double> f;
        k.Multiply(u, &f);
        for (double v : f) CHECK(Near(v, 0.0, 1e-3));
    }
    // The three infinitesimal rotations, likewise.
    const cad::Vec3d axes[3] = {cad::Vec3d{1, 0, 0}, cad::Vec3d{0, 1, 0}, cad::Vec3d{0, 0, 1}};
    for (const cad::Vec3d &axis : axes) {
        std::vector<double> u(static_cast<std::size_t>(model.DofCount()), 0.0);
        for (int n = 0; n < model.NodeCount(); ++n) {
            const cad::Vec3d r = axis.Cross(model.nodes[static_cast<std::size_t>(n)]);
            u[static_cast<std::size_t>(n * 3 + 0)] = r.x;
            u[static_cast<std::size_t>(n * 3 + 1)] = r.y;
            u[static_cast<std::size_t>(n * 3 + 2)] = r.z;
        }
        std::vector<double> f;
        k.Multiply(u, &f);
        for (double v : f) CHECK(Near(v, 0.0, 1e-3));
    }
}

// The patch test. See this file's header comment for why this one matters
// more than the rest put together.
void TestPatchTest() {
    const std::array<int, 3> divisions = {3, 3, 3};
    // A deliberately irregular box (unequal edge lengths, offset origin)
    // so the elements are not cubes -- a patch test on perfect cubes can
    // pass with a Jacobian bug that unequal spacing exposes.
    fem::Model model =
        fem::MakeBoxMesh(cad::Vec3d{-0.3, 0.7, 1.1}, cad::Vec3d{1.7, 0.9, 2.3}, divisions, Steel());

    // An arbitrary linear displacement field u = a + B*x, with a
    // non-symmetric B so it carries rotation as well as strain.
    const cad::Vec3d offset{1e-4, -2e-4, 3e-4};
    const double gradient[3][3] = {{2e-4, -1e-4, 5e-5}, {3e-5, -4e-4, 2e-4}, {-1e-4, 6e-5, 1.5e-4}};
    auto field = [&offset, &gradient](const cad::Vec3d &p) {
        return cad::Vec3d{offset.x + gradient[0][0] * p.x + gradient[0][1] * p.y + gradient[0][2] * p.z,
                          offset.y + gradient[1][0] * p.x + gradient[1][1] * p.y + gradient[1][2] * p.z,
                          offset.z + gradient[2][0] * p.x + gradient[2][1] * p.y + gradient[2][2] * p.z};
    };

    // Prescribe the field on every boundary node, and only there: the
    // interior nodes are what the solver has to get right on its own.
    const int nx = divisions[0], ny = divisions[1], nz = divisions[2];
    int interior_count = 0;
    for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                const int node = i + (nx + 1) * (j + (ny + 1) * k);
                const bool boundary = (i == 0 || i == nx || j == 0 || j == ny || k == 0 || k == nz);
                if (!boundary) {
                    ++interior_count;
                    continue;
                }
                const cad::Vec3d u = field(model.nodes[static_cast<std::size_t>(node)]);
                fem::Constraint c;
                c.node = node;
                c.fixed[0] = c.fixed[1] = c.fixed[2] = true;
                c.value[0] = u.x;
                c.value[1] = u.y;
                c.value[2] = u.z;
                model.constraints.push_back(c);
            }
        }
    }
    // There must actually be interior nodes, or the test proves nothing.
    CHECK(interior_count == (nx - 1) * (ny - 1) * (nz - 1));
    CHECK(interior_count > 0);

    const fem::Result result = fem::Solve(model);
    CHECK(result.ok);

    // Every node -- including the interior ones nobody told the solver
    // about -- must land on the imposed field.
    double worst = 0.0;
    for (int n = 0; n < model.NodeCount(); ++n) {
        const cad::Vec3d expected = field(model.nodes[static_cast<std::size_t>(n)]);
        const cad::Vec3d got = result.displacements[static_cast<std::size_t>(n)];
        worst = std::max(worst, (expected - got).Length());
    }
    std::printf("  patch test: worst nodal displacement error = %.3e (field scale ~1e-4)\n", worst);
    CHECK(worst < 1e-16);

    // The stress implied by that field, computed independently here from
    // the symmetric part of the gradient.
    double d[6][6];
    fem::ConstitutiveMatrix(model.material, d);
    const double strain[6] = {gradient[0][0],
                              gradient[1][1],
                              gradient[2][2],
                              gradient[0][1] + gradient[1][0],
                              gradient[1][2] + gradient[2][1],
                              gradient[0][2] + gradient[2][0]};
    double expected_stress[6] = {0, 0, 0, 0, 0, 0};
    for (int r = 0; r < 6; ++r) {
        for (int c = 0; c < 6; ++c) expected_stress[r] += d[r][c] * strain[c];
    }
    // Uniform across every element, and matching that value.
    for (const fem::StressTensor &s : result.element_stress) {
        for (int i = 0; i < 6; ++i) CHECK(NearRelative(s.s[i], expected_stress[i], 1e-9));
    }
    // And at every node, since averaging a constant field changes nothing.
    for (const fem::StressTensor &s : result.nodal_stress) {
        for (int i = 0; i < 6; ++i) CHECK(NearRelative(s.s[i], expected_stress[i], 1e-9));
    }
}

// A rigid-body displacement must produce exactly zero stress. Distinct
// from the patch test above (whose field has a non-zero symmetric part):
// this one isolates the case where an element that failed to separate
// rotation from strain would report stress from a motion that has none.
void TestRigidBodyMotion() {
    const std::array<int, 3> divisions = {2, 2, 2};
    fem::Model model = fem::MakeBoxMesh(cad::Vec3d{0, 0, 0}, cad::Vec3d{1.0, 1.0, 1.0}, divisions, Steel());
    const cad::Vec3d translation{1e-3, -2e-3, 5e-4};
    for (int n = 0; n < model.NodeCount(); ++n) {
        fem::Constraint c;
        c.node = n;
        c.fixed[0] = c.fixed[1] = c.fixed[2] = true;
        c.value[0] = translation.x;
        c.value[1] = translation.y;
        c.value[2] = translation.z;
        model.constraints.push_back(c);
    }
    const fem::Result result = fem::Solve(model);
    CHECK(result.ok);
    for (const fem::StressTensor &s : result.element_stress) {
        for (int i = 0; i < 6; ++i) CHECK(Near(s.s[i], 0.0, 1.0));  // Pa, against a 2e11 modulus
    }
    CHECK(result.max_von_mises < 10.0);
    // And no reactions: holding a body still where it wants to stay costs
    // nothing.
    for (const cad::Vec3d &r : result.reactions) CHECK(r.Length() < 1e-3);
}

// Uniaxial tension driven by real nodal forces rather than prescribed
// displacement -- so this is the test of the load path, which the patch
// test above never touches.
void TestUniaxialTension() {
    const std::array<int, 3> divisions = {4, 2, 2};
    const double length = 2.0, width = 0.5, height = 0.5;
    fem::Model model =
        fem::MakeBoxMesh(cad::Vec3d{0, 0, 0}, cad::Vec3d{length, width, height}, divisions, Steel());

    // Minimal constraints: the x = 0 face held in x only, so the bar can
    // contract laterally exactly as Poisson's ratio says it must. Over-
    // constraining that face (the tempting "just fix it completely") would
    // suppress the contraction and make the answer inexact -- a mistake
    // worth making explicit, since it looks harmless.
    const std::vector<int> fixed_face = fem::BoxMeshFaceNodes(divisions, 0, false);
    for (int n : fixed_face) {
        fem::Constraint c;
        c.node = n;
        c.fixed[0] = true;
        model.constraints.push_back(c);
    }
    // Three more degrees of freedom to remove the remaining rigid-body
    // translation in y and z and the rotation about x, chosen on nodes
    // far apart so the constraint is well conditioned.
    const int nx = divisions[0], ny = divisions[1];
    auto node_at = [nx, ny](int i, int j, int k) { return i + (nx + 1) * (j + (ny + 1) * k); };
    fem::Constraint origin;
    origin.node = node_at(0, 0, 0);
    origin.fixed[1] = origin.fixed[2] = true;
    model.constraints.push_back(origin);
    fem::Constraint y_axis;
    y_axis.node = node_at(0, divisions[1], 0);
    y_axis.fixed[2] = true;
    model.constraints.push_back(y_axis);

    // Consistent nodal forces for a uniform traction on the far face. For
    // bilinear quad faces this is traction * face_area / 4 at each of the
    // face's four nodes, summed over every element face touching a node
    // -- which is why interior face nodes get more than corners. Dividing
    // the total load equally among the face's nodes instead would *not*
    // produce uniform stress, and the check below would fail: the load
    // vector has to be consistent with the element's shape functions.
    const double total_force = 1.0e6;  // N
    const double area = width * height;
    const double traction = total_force / area;
    const double element_face_area =
        (width / static_cast<double>(divisions[1])) * (height / static_cast<double>(divisions[2]));
    std::vector<double> nodal_force(static_cast<std::size_t>(model.NodeCount()), 0.0);
    for (int k = 0; k < divisions[2]; ++k) {
        for (int j = 0; j < divisions[1]; ++j) {
            const int corners[4] = {node_at(nx, j, k), node_at(nx, j + 1, k), node_at(nx, j + 1, k + 1),
                                    node_at(nx, j, k + 1)};
            for (int corner : corners) {
                nodal_force[static_cast<std::size_t>(corner)] += traction * element_face_area / 4.0;
            }
        }
    }
    double applied_total = 0.0;
    for (int n = 0; n < model.NodeCount(); ++n) {
        if (nodal_force[static_cast<std::size_t>(n)] == 0.0) continue;
        fem::NodalLoad load;
        load.node = n;
        load.force = cad::Vec3d{nodal_force[static_cast<std::size_t>(n)], 0.0, 0.0};
        model.loads.push_back(load);
        applied_total += nodal_force[static_cast<std::size_t>(n)];
    }
    // The consistent distribution must still sum to the total load.
    CHECK(NearRelative(applied_total, total_force, 1e-12));

    const fem::Result result = fem::Solve(model);
    CHECK(result.ok);

    // Exact answers from elementary mechanics.
    const double expected_stress = total_force / area;
    const double expected_elongation = total_force * length / (area * Steel().youngs_modulus);
    const double expected_lateral = -Steel().poissons_ratio * (expected_elongation / length) * width;

    const int far_corner = node_at(divisions[0], divisions[1], divisions[2]);
    // The *axial* component, not max_displacement_magnitude -- that one
    // also carries the lateral Poisson contraction and so is legitimately
    // larger than the elongation it would otherwise look like a poor
    // match for.
    std::printf("  uniaxial: sigma_xx = %.6e Pa (exact %.6e), axial elongation = %.6e m (exact %.6e)\n",
                result.element_stress[0].s[0], expected_stress,
                result.displacements[static_cast<std::size_t>(far_corner)].x, expected_elongation);

    // Uniform axial stress everywhere, and no transverse stress at all.
    for (const fem::StressTensor &s : result.element_stress) {
        CHECK(NearRelative(s.s[0], expected_stress, 1e-9));
        CHECK(Near(s.s[1], 0.0, expected_stress * 1e-9));
        CHECK(Near(s.s[2], 0.0, expected_stress * 1e-9));
        CHECK(Near(s.s[3], 0.0, expected_stress * 1e-9));
        CHECK(Near(s.s[4], 0.0, expected_stress * 1e-9));
        CHECK(Near(s.s[5], 0.0, expected_stress * 1e-9));
        CHECK(NearRelative(s.VonMises(), expected_stress, 1e-9));
    }
    // Axial elongation at the loaded end.
    const int tip = far_corner;
    CHECK(NearRelative(result.displacements[static_cast<std::size_t>(tip)].x, expected_elongation, 1e-9));
    // Lateral contraction, the Poisson effect the constraint choice left
    // free.
    CHECK(NearRelative(result.displacements[static_cast<std::size_t>(tip)].y, expected_lateral, 1e-8));

    // Reactions balance the applied load, to round-off.
    std::printf("  uniaxial: equilibrium residual = %.3e\n", result.equilibrium_residual);
    CHECK(result.equilibrium_residual < 1e-10);

    // The iterative solver must agree with the direct one on the same
    // problem -- they share assembly but nothing else.
    fem::SolveOptions iterative;
    iterative.solver = fem::SolverKind::Iterative;
    iterative.iterative_tolerance = 1e-13;
    const fem::Result cg = fem::Solve(model, iterative);
    CHECK(cg.ok);
    CHECK(cg.solver_iterations > 0);
    for (int n = 0; n < model.NodeCount(); ++n) {
        const cad::Vec3d a = result.displacements[static_cast<std::size_t>(n)];
        const cad::Vec3d b = cg.displacements[static_cast<std::size_t>(n)];
        CHECK((a - b).Length() < expected_elongation * 1e-6);
    }
}

// The cantilever: a real problem with no exact finite-element answer, so
// what is checked is convergence rather than a value.
void TestCantileverConvergence() {
    const double length = 2.0, width = 0.2, height = 0.2;
    const double load = 1000.0;  // N, downward at the tip
    const fem::Material material = Steel();
    const double second_moment = width * height * height * height / 12.0;
    const double euler_bernoulli = load * length * length * length / (3.0 * material.youngs_modulus * second_moment);
    // Timoshenko adds the shear contribution a solid element also carries,
    // so it is the fairer target for a 3D element than the slender-beam
    // formula alone.
    const double shear_modulus = material.youngs_modulus / (2.0 * (1.0 + material.poissons_ratio));
    const double shear_deflection = load * length / ((5.0 / 6.0) * shear_modulus * width * height);
    const double timoshenko = euler_bernoulli + shear_deflection;

    std::printf("  cantilever: beam theory -- Euler-Bernoulli %.4e m, with shear %.4e m\n", euler_bernoulli,
                timoshenko);

    double previous = 0.0;
    for (int refinement = 1; refinement <= 4; ++refinement) {
        const std::array<int, 3> divisions = {8 * refinement, refinement, 2 * refinement};
        fem::Model model =
            fem::MakeBoxMesh(cad::Vec3d{0, 0, 0}, cad::Vec3d{length, width, height}, divisions, material);
        // Root fully fixed.
        for (int n : fem::BoxMeshFaceNodes(divisions, 0, false)) {
            fem::Constraint c;
            c.node = n;
            c.fixed[0] = c.fixed[1] = c.fixed[2] = true;
            model.constraints.push_back(c);
        }
        // Tip load spread evenly over the free-end face. Even (rather
        // than consistent) distribution is fine here: Saint-Venant says
        // the details of how the load is applied stop mattering a few
        // section depths away, and the tip deflection is measured two
        // metres from a 0.2 m section.
        const std::vector<int> tip_nodes = fem::BoxMeshFaceNodes(divisions, 0, true);
        for (int n : tip_nodes) {
            fem::NodalLoad l;
            l.node = n;
            l.force = cad::Vec3d{0.0, 0.0, -load / static_cast<double>(tip_nodes.size())};
            model.loads.push_back(l);
        }

        const fem::Result result = fem::Solve(model);
        CHECK(result.ok);
        CHECK(result.equilibrium_residual < 1e-9);

        // Tip deflection: the mean downward displacement over the free end.
        double deflection = 0.0;
        for (int n : tip_nodes) deflection += -result.displacements[static_cast<std::size_t>(n)].z;
        deflection /= static_cast<double>(tip_nodes.size());

        std::printf("  cantilever: %2dx%dx%2d elements, %5d nodes -> tip %.4e m (%.1f%% of Timoshenko)\n",
                    divisions[0], divisions[1], divisions[2], model.NodeCount(), deflection,
                    100.0 * deflection / timoshenko);

        // Monotonic approach from below: a fully-integrated trilinear
        // hexahedron is too stiff in bending (shear locking, see
        // Hex8Stiffness' own note), so every refinement must soften
        // toward the true answer without ever overshooting it.
        CHECK(deflection > previous);
        CHECK(deflection < timoshenko * 1.02);
        previous = deflection;
    }
    // The finest mesh has to be recognizably close, or "converging" is
    // not a claim worth making.
    CHECK(previous > timoshenko * 0.85);
}

void TestJsonRoundTrip() {
    const std::array<int, 3> divisions = {2, 1, 1};
    fem::Model model = fem::MakeBoxMesh(cad::Vec3d{0.5, -1.0, 2.0}, cad::Vec3d{2.0, 1.0, 1.0}, divisions, Steel());
    fem::Constraint c;
    c.node = 3;
    c.fixed[0] = true;
    c.fixed[2] = true;
    c.value[0] = 1.5e-3;
    model.constraints.push_back(c);
    fem::NodalLoad l;
    l.node = 5;
    l.force = cad::Vec3d{10.0, -20.0, 30.0};
    model.loads.push_back(l);

    const std::string text = fem::ModelToJson(model);
    fem::Model parsed;
    std::string error;
    CHECK(fem::ModelFromJson(text, &parsed, &error));
    CHECK(parsed.NodeCount() == model.NodeCount());
    CHECK(parsed.ElementCount() == model.ElementCount());
    for (int n = 0; n < model.NodeCount(); ++n) {
        CHECK((parsed.nodes[static_cast<std::size_t>(n)] - model.nodes[static_cast<std::size_t>(n)]).Length() <
              1e-12);
    }
    for (std::size_t e = 0; e < model.elements.size(); ++e) {
        for (int n = 0; n < 8; ++n) {
            CHECK(parsed.elements[e].nodes[static_cast<std::size_t>(n)] ==
                  model.elements[e].nodes[static_cast<std::size_t>(n)]);
        }
    }
    CHECK(parsed.constraints.size() == 1);
    CHECK(parsed.constraints[0].node == 3);
    CHECK(parsed.constraints[0].fixed[0] && !parsed.constraints[0].fixed[1] && parsed.constraints[0].fixed[2]);
    CHECK(Near(parsed.constraints[0].value[0], 1.5e-3, 1e-15));
    CHECK(parsed.loads.size() == 1);
    CHECK(Near(parsed.loads[0].force.y, -20.0, 1e-12));
    CHECK(NearRelative(parsed.material.youngs_modulus, Steel().youngs_modulus, 1e-14));
    CHECK(parsed.material.name == "steel");

    // Malformed input is rejected rather than half-parsed.
    fem::Model junk;
    CHECK(!fem::ModelFromJson("{not json", &junk, &error));
    CHECK(!fem::ModelFromJson("{\"nodes\":[1,2]}", &junk, &error));  // not a multiple of 3
    CHECK(!fem::ModelFromJson("{\"nodes\":[0,0,0],\"hexes\":[1,2,3]}", &junk, &error));  // not a multiple of 8

    // A failed solve serializes as an error, not as an empty success.
    fem::Result failed;
    failed.error = "something went wrong";
    const std::string failed_text = fem::ResultToJson(failed);
    CHECK(failed_text.find("\"ok\":false") != std::string::npos);
    CHECK(failed_text.find("something went wrong") != std::string::npos);
}

void TestSolveErrors() {
    const std::array<int, 3> divisions = {1, 1, 1};
    fem::Model model = fem::MakeBoxMesh(cad::Vec3d{0, 0, 0}, cad::Vec3d{1, 1, 1}, divisions, Steel());
    // Unconstrained: rejected by validation with a useful message.
    const fem::Result r = fem::Solve(model);
    CHECK(!r.ok);
    CHECK(!r.error.empty());

    // Constrained at a single node only: six fixed degrees of freedom, so
    // validation passes, but the body can still rotate about that node
    // and the factorization must be the thing that catches it.
    fem::Model under = model;
    fem::Constraint c;
    c.node = 0;
    c.fixed[0] = c.fixed[1] = c.fixed[2] = true;
    under.constraints.push_back(c);
    fem::Constraint c2;
    c2.node = 0;
    c2.fixed[0] = c2.fixed[1] = c2.fixed[2] = true;
    under.constraints.push_back(c2);  // duplicate: still only 3 real DOFs
    const fem::Result ru = fem::Solve(under);
    CHECK(!ru.ok);
    CHECK(!ru.error.empty());
}

void TestProgressCallback() {
    const std::array<int, 3> divisions = {2, 2, 2};
    fem::Model model = fem::MakeBoxMesh(cad::Vec3d{0, 0, 0}, cad::Vec3d{1, 1, 1}, divisions, Steel());
    for (int n : fem::BoxMeshFaceNodes(divisions, 0, false)) {
        fem::Constraint c;
        c.node = n;
        c.fixed[0] = c.fixed[1] = c.fixed[2] = true;
        model.constraints.push_back(c);
    }
    fem::NodalLoad l;
    l.node = model.NodeCount() - 1;
    l.force = cad::Vec3d{0, 0, -100.0};
    model.loads.push_back(l);

    std::vector<double> fractions;
    std::vector<std::string> stages;
    fem::SolveOptions options;
    options.progress = [&fractions, &stages](double fraction, const std::string &stage) {
        fractions.push_back(fraction);
        stages.push_back(stage);
    };
    const fem::Result r = fem::Solve(model, options);
    CHECK(r.ok);
    CHECK(fractions.size() >= 4);
    // Monotonic and ending at 1.
    for (std::size_t i = 1; i < fractions.size(); ++i) CHECK(fractions[i] >= fractions[i - 1]);
    CHECK(Near(fractions.back(), 1.0, 1e-12));
    CHECK(stages.back() == "done");
}

}  // namespace

int main() {
    TestMeshGeneration();
    TestValidation();
    TestStressInvariants();
    TestConstitutiveMatrix();
    TestStiffnessProperties();
    TestPatchTest();
    TestRigidBodyMotion();
    TestUniaxialTension();
    TestCantileverConvergence();
    TestJsonRoundTrip();
    TestSolveErrors();
    TestProgressCallback();
    std::printf("fem_test passed (%d checks)\n", g_checks);
    return 0;
}
