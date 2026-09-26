// Part H.3: assembly.

#include "fem_assemble.h"
#include "num_sparse.h"
#include "fem_solve.h"
#include "num_sparse.h"

#include <algorithm>
#include <cstdint>
#include <utility>
#include <cmath>
#include <cstdio>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char *what, int line) {
    ++checks;
    if (ok) return;
    std::printf("CHECK FAILED: %s at %s:%d\n", what, __FILE__, line);
    ++failures;
}

#define CHECK(x) Check((x), #x, __LINE__)
#define CHECK_MESSAGE(x, message) Check((x), (std::string(#x) + ": " + (message)).c_str(), __LINE__)

using cad::Vec3d;
using fem::AnalysisModel;
using fem::AssemblyOptions;
using fem::BoundElement;
using fem::ConstraintTerm;
using fem::ElementShape;
using fem::MpcMethod;
using fem::MultiPointConstraint;
using fem::SparsityPattern;
using fem::StudyMaterial;
using fem::System;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// A block of hexahedra, built here rather than meshed: a structured mesh
// has no mesher in it, so a failure is unambiguously the assembly's.
AnalysisModel Block(const Vec3d &size, int nx, int ny, int nz) {
    AnalysisModel model;
    StudyMaterial steel;
    steel.name = "steel";
    model.materials.push_back(steel);
    for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                model.nodes.push_back(Vec3d{size.x * i / nx, size.y * j / ny, size.z * k / nz});
            }
        }
    }
    auto at = [&](int i, int j, int k) { return (k * (ny + 1) + j) * (nx + 1) + i; };
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                BoundElement element;
                element.shape = ElementShape::Hex8;
                element.nodes = {at(i, j, k),         at(i + 1, j, k),
                                 at(i + 1, j + 1, k), at(i, j + 1, k),
                                 at(i, j, k + 1),     at(i + 1, j, k + 1),
                                 at(i + 1, j + 1, k + 1), at(i, j + 1, k + 1)};
                model.elements.push_back(element);
            }
        }
    }
    return model;
}

// --- The pattern against the fill -----------------------------------------

void TestPattern() {
    std::printf("the sparsity pattern:\n");
    const AnalysisModel model = Block(Vec3d{3, 2, 2}, 3, 2, 2);
    SparsityPattern pattern;
    std::string error;
    CHECK_MESSAGE(pattern.Build(model, &error), error);
    CHECK(pattern.Dofs() == model.NodeCount() * 3);

    num::SparseMatrix k;
    CHECK_MESSAGE(fem::AssembleStiffness(model, pattern, &k, &error), error);
    std::printf("  %d dofs, %d non-zeros, %.1f%% dense\n", pattern.Dofs(), pattern.NonZeros(),
                100.0 * pattern.NonZeros() /
                    (static_cast<double>(pattern.Dofs()) * pattern.Dofs()));

    // THE PATTERN AND THE FILL ARE TWO DERIVATIONS OF THE SAME POSITIONS.
    // The pattern comes from connectivity -- two degrees of freedom share
    // an entry when an element holds them both -- and the fill comes from
    // the element stiffness matrices. They must agree exactly, and an
    // element stiffness that is structurally zero where connectivity says
    // it should not be is the one direction this allows: a zero entry
    // that the pattern reserved.
    std::set<std::pair<int, int>> from_connectivity;
    for (const BoundElement &element : model.elements) {
        for (const int a : element.nodes) {
            for (const int b : element.nodes) {
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 3; ++j) {
                        from_connectivity.insert({a * 3 + i, b * 3 + j});
                    }
                }
            }
        }
    }
    for (int row = 0; row < pattern.Dofs(); ++row) from_connectivity.insert({row, row});
    CHECK(static_cast<int>(from_connectivity.size()) == pattern.NonZeros());
    for (const auto &entry : from_connectivity) {
        CHECK(pattern.Find(entry.first, entry.second) >= 0);
    }
    // And nothing outside it: an entry the pattern has that connectivity
    // does not would be a fill that could go somewhere it should not.
    for (int row = 0; row < pattern.Dofs(); ++row) {
        for (int at = pattern.RowStart()[Idx(row)]; at < pattern.RowStart()[Idx(row + 1)]; ++at) {
            CHECK(from_connectivity.count({row, pattern.ColIndex()[Idx(at)]}) == 1);
        }
    }
    // Sorted within each row, which Find depends on and a binary search
    // would silently get wrong on unsorted data.
    for (int row = 0; row < pattern.Dofs(); ++row) {
        for (int at = pattern.RowStart()[Idx(row)] + 1; at < pattern.RowStart()[Idx(row + 1)];
             ++at) {
            CHECK(pattern.ColIndex()[Idx(at)] > pattern.ColIndex()[Idx(at - 1)]);
        }
    }

    // FILLING THE SAME PATTERN TWICE GIVES THE SAME NUMBERS, BIT FOR BIT.
    // That is the property a nonlinear solve depends on: the pattern is
    // built once and filled every iteration, so a fill that depended on
    // anything but the model would drift.
    num::SparseMatrix again;
    CHECK_MESSAGE(fem::AssembleStiffness(model, pattern, &again, &error), error);
    CHECK(again.NonZeros() == k.NonZeros());
    for (int i = 0; i < k.NonZeros(); ++i) {
        CHECK(again.Values()[Idx(i)] == k.Values()[Idx(i)]);
    }

    // Symmetric, and annihilating the six rigid motions -- the global
    // form of the element rank check, and what says the element matrices
    // were scattered to the right places. A mis-indexed assembly gives a
    // symmetric matrix too, so symmetry alone would not catch it.
    CHECK(k.IsSymmetric(1e-9));
    double largest = 0.0;
    for (const double value : k.Values()) largest = std::max(largest, std::fabs(value));
    for (int mode = 0; mode < 6; ++mode) {
        std::vector<double> u(Idx(pattern.Dofs()), 0.0);
        for (int node = 0; node < model.NodeCount(); ++node) {
            const Vec3d &p = model.nodes[Idx(node)];
            Vec3d motion{};
            if (mode < 3) {
                double component[3] = {0, 0, 0};
                component[mode] = 1.0;
                motion = Vec3d{component[0], component[1], component[2]};
            } else {
                const Vec3d about = mode == 3 ? Vec3d{1, 0, 0}
                                              : (mode == 4 ? Vec3d{0, 1, 0} : Vec3d{0, 0, 1});
                motion = about.Cross(p);
            }
            u[Idx(node * 3 + 0)] = motion.x;
            u[Idx(node * 3 + 1)] = motion.y;
            u[Idx(node * 3 + 2)] = motion.z;
        }
        std::vector<double> force;
        k.Multiply(u, &force);
        double worst = 0.0;
        for (const double value : force) worst = std::max(worst, std::fabs(value));
        CHECK(worst < largest * 1e-9);
    }
}

// --- Elimination, against an answer that can be written down --------------

void TestBarInTension() {
    std::printf("a bar pulled along its length:\n");
    // A prism held at one end and pulled at the other. Uniform stress, so
    // the extension is exactly PL/AE whatever the mesh -- the one elastic
    // problem a first-order mesh gets exactly right, which makes it the
    // right first test of the assembly rather than of the elements.
    const double length = 4.0;
    const double width = 1.0;
    const double height = 2.0;
    const double pull = 1e6;
    AnalysisModel model = Block(Vec3d{length, width, height}, 4, 2, 2);
    const double e = model.materials[0].youngs_modulus.At(20.0);

    std::vector<int> held;
    std::vector<int> pulled;
    for (int node = 0; node < model.NodeCount(); ++node) {
        if (model.nodes[Idx(node)].x == 0.0) held.push_back(node);
        if (model.nodes[Idx(node)].x == length) pulled.push_back(node);
    }
    for (const int node : held) {
        fem::Constraint constraint;
        constraint.node = node;
        // Held against sliding along the bar, and against rigid motion by
        // one corner only -- so the cross-section is free to contract,
        // which is what makes the answer exactly PL/AE rather than
        // something stiffer.
        constraint.fixed[0] = true;
        if (model.nodes[Idx(node)].y == 0.0) constraint.fixed[1] = true;
        if (model.nodes[Idx(node)].z == 0.0) constraint.fixed[2] = true;
        model.constraints.push_back(constraint);
    }
    // THE LOAD IS SPREAD BY TRIBUTARY AREA, NOT DIVIDED EQUALLY. Equal
    // shares are a non-uniform traction -- a corner node gets four times
    // the traction an interior one does -- so the stress is not uniform
    // and the extension is not PL/AE. It came out five percent soft and
    // the end bulged by a quarter of the mean, which is Part H.2's point
    // about consistent loads arriving from the other direction. For a
    // structured grid the consistent share of a uniform traction is
    // exactly the tributary area, which is what this is.
    for (const int node : pulled) {
        const Vec3d &p = model.nodes[Idx(node)];
        const double share_y = (p.y == 0.0 || p.y == width) ? 0.5 : 1.0;
        const double share_z = (p.z == 0.0 || p.z == height) ? 0.5 : 1.0;
        const double area = (width / 2) * (height / 2) * share_y * share_z;
        model.loads.push_back(fem::NodalLoad{node, Vec3d{pull * area / (width * height), 0, 0}});
    }

    System system;
    std::string error;
    CHECK_MESSAGE(fem::BuildSystem(model, {}, {}, &system, &error), error);
    CHECK(system.multipliers == 0);
    num::SparseLDLT factor;
    CHECK_MESSAGE(factor.Factorize(system.matrix), factor.Error());
    std::vector<double> reduced;
    CHECK(factor.Solve(system.rhs, &reduced));
    std::vector<double> displacement;
    system.Expand(reduced, &displacement);

    double tip = 0.0;
    for (const int node : pulled) tip += displacement[Idx(node * 3)];
    tip /= static_cast<double>(pulled.size());
    const double exact = pull * length / (width * height * e);
    std::printf("  extension %.6e, exactly %.6e\n", tip, exact);
    CHECK(std::fabs(tip - exact) < exact * 1e-9);

    // THE REACTIONS BALANCE THE LOAD. The cheapest global check there is
    // that a solve was right, and it tests the assembly rather than the
    // solver: a mis-scattered element matrix gives a solution that
    // satisfies its own equations and does not balance.
    std::vector<Vec3d> reactions;
    SparsityPattern pattern;
    CHECK_MESSAGE(pattern.Build(model, &error), error);
    CHECK_MESSAGE(fem::Reactions(model, pattern, displacement, &reactions, &error), error);
    Vec3d total{};
    for (const Vec3d &reaction : reactions) total = total + reaction;
    std::printf("  reactions sum to (%.3e, %.3e, %.3e) against a load of %.3e\n", total.x, total.y,
                total.z, pull);
    // They BALANCE the load rather than vanish: at a free degree of
    // freedom K*u - f is zero, and at a held one it is the force the
    // support supplied, so the sum is the applied load negated.
    CHECK((total + Vec3d{pull, 0, 0}).Length() < pull * 1e-9);
}

// --- Multi-point constraints ----------------------------------------------

void TestMultiPoint() {
    std::printf("multi-point constraints, three ways:\n");
    const double length = 4.0;
    const double pull = 1e6;

    // The same bar, but the far end is required to stay flat: every node
    // on it moves by the same amount in x. That is what a rigid platen
    // pressing on it does, and it cannot be said with single-point
    // constraints, because the amount is not known in advance.
    auto build = [&]() {
        AnalysisModel model = Block(Vec3d{length, 1, 2}, 4, 2, 2);
        for (int node = 0; node < model.NodeCount(); ++node) {
            const Vec3d &p = model.nodes[Idx(node)];
            if (p.x != 0.0) continue;
            fem::Constraint constraint;
            constraint.node = node;
            constraint.fixed[0] = true;
            if (p.y == 0.0) constraint.fixed[1] = true;
            if (p.z == 0.0) constraint.fixed[2] = true;
            model.constraints.push_back(constraint);
        }
        std::vector<int> far;
        for (int node = 0; node < model.NodeCount(); ++node) {
            if (model.nodes[Idx(node)].x == length) far.push_back(node);
        }
        for (const int node : far) {
            model.loads.push_back(
                fem::NodalLoad{node, Vec3d{pull / static_cast<double>(far.size()), 0, 0}});
        }
        return std::make_pair(model, far);
    };

    auto solve = [&](MpcMethod method, const std::vector<MultiPointConstraint> &constraints,
                     double *out_spread) {
        const auto built = build();
        const AnalysisModel &model = built.first;
        System system;
        std::string error;
        AssemblyOptions options;
        options.mpc = method;
        if (!fem::BuildSystem(model, constraints, options, &system, &error)) {
            std::printf("    %s\n", error.c_str());
            ++failures;
            return 0.0;
        }
        num::SparseLDLT factor;
        if (!factor.Factorize(system.matrix, system.multipliers > 0 ? num::Ordering::Natural
                                                                   : num::Ordering::MinimumDegree)) {
            std::printf("    factorization failed: %s\n", factor.Error().c_str());
            ++failures;
            return 0.0;
        }
        std::vector<double> reduced;
        if (!factor.Solve(system.rhs, &reduced)) {
            ++failures;
            return 0.0;
        }
        std::vector<double> displacement;
        system.Expand(reduced, &displacement);
        double lowest = 1e30;
        double highest = -1e30;
        double mean = 0.0;
        for (const int node : built.second) {
            const double value = displacement[Idx(node * 3)];
            lowest = std::min(lowest, value);
            highest = std::max(highest, value);
            mean += value;
        }
        mean /= static_cast<double>(built.second.size());
        *out_spread = highest - lowest;
        return mean;
    };

    // The constraint rows: every far node tied to the first of them.
    const auto built = build();
    std::vector<MultiPointConstraint> tied;
    for (std::size_t i = 1; i < built.second.size(); ++i) {
        MultiPointConstraint row;
        row.label = "the far end stays flat";
        row.terms.push_back(ConstraintTerm{built.second[i], 0, 1.0});
        row.terms.push_back(ConstraintTerm{built.second[0], 0, -1.0});
        tied.push_back(row);
    }

    double free_spread = 0.0;
    double lagrange_spread = 0.0;
    double penalty_spread = 0.0;
    const double unconstrained = solve(MpcMethod::Lagrange, {}, &free_spread);
    const double lagrange = solve(MpcMethod::Lagrange, tied, &lagrange_spread);
    const double penalty = solve(MpcMethod::Penalty, tied, &penalty_spread);
    std::printf("  %-24s mean %.6e, spread across the end %.2e\n", "unconstrained", unconstrained,
                free_spread);
    std::printf("  %-24s mean %.6e, spread across the end %.2e\n", "Lagrange multipliers",
                lagrange, lagrange_spread);
    std::printf("  %-24s mean %.6e, spread across the end %.2e\n", "penalty", penalty,
                penalty_spread);

    // The unconstrained end is not flat: a bar pulled by nodal forces
    // bulges, so there is something for the constraint to do. Without
    // this the rest of the test would pass on a constraint that did
    // nothing at all.
    CHECK(free_spread > std::fabs(unconstrained) * 1e-3);
    // THE CONSTRAINT IS SATISFIED EXACTLY BY LAGRANGE AND NEARLY BY
    // PENALTY, and the difference between the two is exactly what the
    // choice between them costs.
    CHECK(lagrange_spread < std::fabs(lagrange) * 1e-12);
    CHECK(penalty_spread < std::fabs(penalty) * 1e-6);
    CHECK(penalty_spread > lagrange_spread);
    // And they agree on the answer, which is the check that neither is
    // merely self-consistent.
    CHECK(std::fabs(lagrange - penalty) < std::fabs(lagrange) * 1e-6);
    // Holding the end flat is a restraint, so the bar cannot extend
    // quite as far as it did.
    CHECK(lagrange < unconstrained);

    // A CONSTRAINT THAT CANNOT BE MET IS REFUSED. Two prescribed degrees
    // of freedom tied to each other with different values is not a
    // constraint, it is a contradiction, and a penalty method would
    // happily return the average.
    AnalysisModel model = built.first;
    MultiPointConstraint impossible;
    impossible.label = "impossible";
    impossible.terms.push_back(ConstraintTerm{model.constraints[0].node, 0, 1.0});
    impossible.value = 1.0;
    System system;
    std::string error;
    CHECK(!fem::BuildSystem(model, {impossible}, {}, &system, &error));
    CHECK(error.find("over-constrained") != std::string::npos);
    std::printf("  %s\n", error.c_str());
}

// --- Rigid links ----------------------------------------------------------

void TestRigidLink() {
    std::printf("a rigid link:\n");
    AnalysisModel model = Block(Vec3d{4, 1, 2}, 4, 1, 2);
    for (int node = 0; node < model.NodeCount(); ++node) {
        if (model.nodes[Idx(node)].x != 0.0) continue;
        fem::Constraint constraint;
        constraint.node = node;
        constraint.fixed[0] = constraint.fixed[1] = constraint.fixed[2] = true;
        model.constraints.push_back(constraint);
    }
    std::vector<int> far;
    for (int node = 0; node < model.NodeCount(); ++node) {
        if (model.nodes[Idx(node)].x == 4.0) far.push_back(node);
    }
    // The whole load on one node of the far face, with the rest of the
    // face rigidly linked to it -- which is how a bolt or a bearing is
    // modelled, and spreads the load without inventing a stress
    // concentration at the one node it was applied to.
    model.loads.push_back(fem::NodalLoad{far[0], Vec3d{0, 0, -1e5}});

    fem::RigidLink link;
    link.label = "the far face moves with its centre";
    link.controlling = far[0];
    for (std::size_t i = 1; i < far.size(); ++i) link.dependent.push_back(far[i]);
    std::vector<MultiPointConstraint> rows;
    std::string error;
    CHECK_MESSAGE(fem::ExpandRigidLink(model, link, &rows, &error), error);
    CHECK(static_cast<int>(rows.size()) == 3 * (static_cast<int>(far.size()) - 1));

    System system;
    CHECK_MESSAGE(fem::BuildSystem(model, rows, {}, &system, &error), error);
    num::SparseLDLT factor;
    CHECK_MESSAGE(factor.Factorize(system.matrix, num::Ordering::Natural), factor.Error());
    std::vector<double> reduced;
    CHECK(factor.Solve(system.rhs, &reduced));
    std::vector<double> displacement;
    system.Expand(reduced, &displacement);

    // THE LINKED FACE MOVED AS ONE. Every node of it has the same
    // displacement, in all three directions, which is what the link says
    // and is not what a face loaded at one node does on its own.
    Vec3d first{displacement[Idx(far[0] * 3)], displacement[Idx(far[0] * 3 + 1)],
                displacement[Idx(far[0] * 3 + 2)]};
    double worst = 0.0;
    for (const int node : far) {
        const Vec3d at{displacement[Idx(node * 3)], displacement[Idx(node * 3 + 1)],
                       displacement[Idx(node * 3 + 2)]};
        worst = std::max(worst, (at - first).Length());
    }
    std::printf("  the far face deflected %.6e, and moved as one to within %.2e\n", -first.z,
                worst);
    CHECK(first.z < 0.0);
    CHECK(worst < first.Length() * 1e-12);

    // A link naming a node that is not there is refused rather than
    // silently skipped.
    fem::RigidLink broken = link;
    broken.dependent.push_back(999999);
    rows.clear();
    CHECK(!fem::ExpandRigidLink(model, broken, &rows, &error));
}

// --- The old solver, now on the element library ----------------------------

void TestHex8AgreesWithTheLibrary() {
    std::printf("the Part 0.6 solver against the element library:\n");
    // fem_solve.cpp had its own copy of Hex8's shape functions. They are
    // gone; Hex8Stiffness now calls the library. Checking that the
    // numbers did not move is the whole of what that change had to
    // preserve, and it is checked on a *distorted* element, because an
    // undistorted one agrees under almost any mistake.
    fem::Model old;
    old.material.youngs_modulus = 210e9;
    old.material.poissons_ratio = 0.3;
    const Vec3d reference[8] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
    std::vector<Vec3d> nodes;
    for (int i = 0; i < 8; ++i) {
        const std::uint64_t h = static_cast<std::uint64_t>(i + 1) * 0x9E3779B97F4A7C15ull;
        auto unit = [&](int shift) {
            return static_cast<double>((h >> shift) & 0xFFFF) / 65535.0 - 0.5;
        };
        nodes.push_back(reference[i] + Vec3d{unit(0), unit(16), unit(32)} * 0.2);
        old.nodes.push_back(nodes.back());
    }
    fem::Element element;
    for (int i = 0; i < 8; ++i) element.nodes[Idx(i)] = i;
    old.elements.push_back(element);

    double theirs[24][24];
    fem::Hex8Stiffness(old, element, theirs);
    std::vector<double> ours;
    std::string error;
    CHECK_MESSAGE(fem::ElementStiffness(ElementShape::Hex8, nodes, old.material, {}, &ours, &error),
                  error);
    double worst = 0.0;
    double largest = 0.0;
    for (int i = 0; i < 24; ++i) {
        for (int j = 0; j < 24; ++j) {
            worst = std::max(worst, std::fabs(theirs[i][j] - ours[Idx(i * 24 + j)]));
            largest = std::max(largest, std::fabs(theirs[i][j]));
        }
    }
    std::printf("  worst disagreement %.2e of %.3g\n", worst, largest);
    CHECK(worst < largest * 1e-12);
}

// --- Solving one iteratively (Part H.5) -----------------------------------

void TestIterativeSolve() {
    std::printf("the same model, factored and iterated:\n");
    // THE NEAR-NULL SPACE IS THE WHOLE DIFFERENCE for elasticity.
    // Multigrid works by representing, on a coarse level, the error the
    // smoother cannot reduce; for an elliptic operator that error looks
    // locally like the operator's near-null space, which for elasticity
    // is the six rigid-body modes and not the constant vector. Handing it
    // the constant alone is the standard way to build an algebraic
    // multigrid for a structural problem that does not converge and to
    // conclude that multigrid does not work here.
    std::printf("  %-14s %-9s %-9s %-11s %-11s\n", "elements", "dofs", "direct", "cg + ic(0)",
                "cg + amg");
    std::vector<int> cholesky_counts;
    std::vector<int> amg_counts;
    for (const int m : {4, 6, 8, 10}) {
        AnalysisModel model = Block(Vec3d{4, 2, 2}, m * 2, m, m);
        for (int node = 0; node < model.NodeCount(); ++node) {
            if (model.nodes[Idx(node)].x != 0.0) continue;
            fem::Constraint constraint;
            constraint.node = node;
            constraint.fixed[0] = constraint.fixed[1] = constraint.fixed[2] = true;
            model.constraints.push_back(constraint);
        }
        for (int node = 0; node < model.NodeCount(); ++node) {
            if (model.nodes[Idx(node)].x != 4.0) continue;
            model.loads.push_back(fem::NodalLoad{node, Vec3d{0, 0, -1e3}});
        }
        System system;
        std::string error;
        CHECK_MESSAGE(fem::BuildSystem(model, {}, {}, &system, &error), error);

        num::SparseLDLT factor;
        CHECK_MESSAGE(factor.Factorize(system.matrix), factor.Error());
        std::vector<double> direct;
        CHECK(factor.Solve(system.rhs, &direct));

        // The six rigid-body modes, restricted to the degrees of freedom
        // the system actually carries.
        std::vector<std::vector<double>> rigid;
        for (int mode = 0; mode < 6; ++mode) {
            std::vector<double> full(Idx(model.NodeCount() * 3), 0.0);
            for (int node = 0; node < model.NodeCount(); ++node) {
                const Vec3d &p = model.nodes[Idx(node)];
                Vec3d motion{};
                if (mode < 3) {
                    double component[3] = {0, 0, 0};
                    component[mode] = 1.0;
                    motion = Vec3d{component[0], component[1], component[2]};
                } else {
                    const Vec3d about = mode == 3 ? Vec3d{1, 0, 0}
                                                  : (mode == 4 ? Vec3d{0, 1, 0} : Vec3d{0, 0, 1});
                    motion = about.Cross(p);
                }
                full[Idx(node * 3 + 0)] = motion.x;
                full[Idx(node * 3 + 1)] = motion.y;
                full[Idx(node * 3 + 2)] = motion.z;
            }
            std::vector<double> reduced(system.dof_of_row.size(), 0.0);
            for (std::size_t row = 0; row < system.dof_of_row.size(); ++row) {
                if (system.dof_of_row[row] < 0) continue;
                reduced[row] = full[Idx(system.dof_of_row[row])];
            }
            rigid.push_back(reduced);
        }

        int counts[2] = {0, 0};
        std::vector<double> iterative;
        for (int which = 0; which < 2; ++which) {
            num::IterativeOptions options;
            options.preconditioner = which == 0 ? num::Preconditioner::IncompleteCholesky
                                                : num::Preconditioner::AlgebraicMultigrid;
            options.multigrid.near_null_space = rigid;
            options.multigrid.block_size = 3;
            // A HIERARCHY OF THE SAME DEPTH AT EVERY SIZE, which the
            // default coarse limit does not give at these sizes and which
            // the comparison needs. With two levels the "coarse solve" is
            // a direct factorization of a quarter-sized problem, so the
            // preconditioner is very nearly exact and the iteration count
            // flatters it; the count then jumps when a third level
            // appears, and the jump looks like a failure of mesh
            // independence when it is a change of method. Forcing three
            // levels throughout compares like with like.
            options.multigrid.coarse_limit = 60;
            options.tolerance = 1e-10;
            options.max_iterations = 5000;
            std::vector<double> x;
            const num::IterativeResult result =
                num::ConjugateGradient(system.matrix, system.rhs, &x, options);
            CHECK_MESSAGE(result.converged, result.message);
            counts[which] = result.iterations;
            if (which == 1) iterative = x;
        }

        // AND THEY AGREE. Two solvers with nothing in common but the
        // matrix: one factors it exactly, the other never forms a factor
        // at all. A preconditioner that is subtly wrong slows convergence
        // and does not change the answer, so this checks the solver
        // rather than the preconditioner -- which is why both are here.
        double worst = 0.0;
        double scale = 0.0;
        for (std::size_t i = 0; i < direct.size(); ++i) {
            worst = std::max(worst, std::fabs(direct[i] - iterative[i]));
            scale = std::max(scale, std::fabs(direct[i]));
        }
        std::printf("  %-14d %-9d %-9s %-11d %-11d  (agree to %.1e of %.1e)\n",
                    model.ElementCount(), system.Size(), "exact", counts[0], counts[1], worst,
                    scale);
        CHECK(worst < scale * 1e-6);
        cholesky_counts.push_back(counts[0]);
        amg_counts.push_back(counts[1]);
    }
    // The same statement as on the Laplacian, on the problem it is for:
    // the local preconditioner's count grows with the mesh and
    // multigrid's flattens once the hierarchy is genuinely multilevel.
    std::printf("  ic(0) went %d -> %d as the mesh refined; amg went %d -> %d\n",
                cholesky_counts.front(), cholesky_counts.back(), amg_counts.front(),
                amg_counts.back());
    CHECK(cholesky_counts.back() > cholesky_counts.front() * 3 / 2);
    CHECK(amg_counts.back() < cholesky_counts.back());
    // Flat across the last two refinements, where incomplete Cholesky is
    // still climbing. Asserted on the tail rather than on the whole range
    // because the smallest problem here has barely enough unknowns for
    // three levels, and a method for large problems should not be judged
    // on the smallest one it will accept.
    const std::size_t last = amg_counts.size() - 1;
    CHECK(amg_counts[last] < amg_counts[last - 1] * 6 / 5);
    CHECK(cholesky_counts[last] > cholesky_counts[last - 1] * 11 / 10);
}

}  // namespace

int main() {
    TestPattern();
    TestBarInTension();
    TestMultiPoint();
    TestRigidLink();
    TestHex8AgreesWithTheLibrary();
    TestIterativeSolve();
    if (failures != 0) {
        std::printf("fem_assemble_test FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    std::printf("fem_assemble_test passed (%d checks)\n", checks);
    return 0;
}
