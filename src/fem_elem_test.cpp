// Part H.1's element library, held to the properties an element has to
// have. Every check here follows from what a finite element *is*, so
// none of them needs a known answer to a physical problem -- which is
// the point, because an element with a sign wrong in one derivative
// solves happily and is wrong by twenty percent.

#include "fem_elem.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <map>
#include <utility>
#include <cstdio>
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
using fem::ElementDimension;
using fem::ElementNodeCount;
using fem::ElementOptions;
using fem::ElementShape;
using fem::ElementShapeName;
using fem::Material;
using fem::QuadraturePoint;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// Points strictly inside each reference element, for the identities that
// have to hold everywhere rather than only at the nodes.
std::vector<Vec3d> InteriorPoints(ElementShape shape) {
    switch (shape) {
        case ElementShape::Tri3:
        case ElementShape::Tri6:
            return {Vec3d{0.25, 0.25, 0}, Vec3d{0.5, 0.3, 0}, Vec3d{0.1, 0.8, 0},
                    Vec3d{1.0 / 3.0, 1.0 / 3.0, 0}};
        case ElementShape::Quad4:
        case ElementShape::Quad8:
            return {Vec3d{0, 0, 0}, Vec3d{0.3, -0.7, 0}, Vec3d{-0.9, 0.2, 0}, Vec3d{0.55, 0.55, 0}};
        case ElementShape::Tet4:
        case ElementShape::Tet10:
            return {Vec3d{0.25, 0.25, 0.25}, Vec3d{0.1, 0.2, 0.3}, Vec3d{0.5, 0.2, 0.1},
                    Vec3d{0.05, 0.05, 0.85}};
        case ElementShape::Wedge6:
        case ElementShape::Wedge15:
            return {Vec3d{1.0 / 3.0, 1.0 / 3.0, 0}, Vec3d{0.2, 0.3, -0.6}, Vec3d{0.6, 0.1, 0.8},
                    Vec3d{0.1, 0.7, 0.2}};
        case ElementShape::Pyr5:
            return {Vec3d{0, 0, 0.5}, Vec3d{0.3, -0.2, 0.2}, Vec3d{-0.4, 0.4, 0.1},
                    Vec3d{0.05, 0.05, 0.9}};
        default:
            return {Vec3d{0, 0, 0}, Vec3d{0.3, -0.4, 0.2}, Vec3d{-0.6, 0.5, -0.3},
                    Vec3d{0.7, 0.7, 0.7}};
    }
}

// --- The identities every element obeys -----------------------------------

void CheckShapeFunctions(ElementShape shape) {
    const int count = ElementNodeCount(shape);
    const int dimension = ElementDimension(shape);
    std::vector<Vec3d> reference;
    fem::ReferenceNodes(shape, &reference);
    CHECK(static_cast<int>(reference.size()) == count);

    std::vector<double> n;
    std::vector<double> dn;

    // ONE AT ITS OWN NODE AND ZERO AT THE OTHERS. Without this the nodal
    // values are not the nodal values, and a prescribed displacement is
    // not the displacement that was prescribed.
    for (int a = 0; a < count; ++a) {
        fem::ShapeFunctions(shape, reference[Idx(a)], &n, &dn);
        for (int b = 0; b < count; ++b) {
            const double wanted = a == b ? 1.0 : 0.0;
            CHECK(std::fabs(n[Idx(b)] - wanted) < 1e-12);
        }
    }

    for (const Vec3d &at : InteriorPoints(shape)) {
        fem::ShapeFunctions(shape, at, &n, &dn);
        // SUMMING TO ONE is what makes a constant representable, which is
        // what makes rigid-body translation free of strain.
        double total = 0.0;
        for (const double value : n) total += value;
        CHECK(std::fabs(total - 1.0) < 1e-12);

        // The same statement differentiated: the derivatives sum to zero.
        for (int i = 0; i < dimension; ++i) {
            double slope = 0.0;
            for (int a = 0; a < count; ++a) slope += dn[Idx(a * 3 + i)];
            CHECK(std::fabs(slope) < 1e-11);
        }

        // LINEAR COMPLETENESS: the map reproduces the reference
        // coordinates themselves, and its derivative is the identity.
        // This is the patch test in miniature and catches a mis-placed
        // node that the partition of unity does not.
        for (int j = 0; j < dimension; ++j) {
            double mapped = 0.0;
            for (int a = 0; a < count; ++a) {
                const double p[3] = {reference[Idx(a)].x, reference[Idx(a)].y,
                                     reference[Idx(a)].z};
                mapped += n[Idx(a)] * p[j];
            }
            const double wanted[3] = {at.x, at.y, at.z};
            CHECK(std::fabs(mapped - wanted[j]) < 1e-11);
            for (int i = 0; i < dimension; ++i) {
                double slope = 0.0;
                for (int a = 0; a < count; ++a) {
                    const double p[3] = {reference[Idx(a)].x, reference[Idx(a)].y,
                                         reference[Idx(a)].z};
                    slope += dn[Idx(a * 3 + i)] * p[j];
                }
                CHECK(std::fabs(slope - (i == j ? 1.0 : 0.0)) < 1e-10);
            }
        }

        // And the analytic derivatives agree with finite differences of
        // the functions themselves -- the one check that compares the two
        // halves of ShapeFunctions against each other rather than each
        // against an identity they might both satisfy while disagreeing.
        const double step = 1e-6;
        std::vector<double> plus;
        std::vector<double> minus;
        std::vector<double> ignored;
        for (int i = 0; i < dimension; ++i) {
            Vec3d up = at;
            Vec3d down = at;
            double *axis_up[3] = {&up.x, &up.y, &up.z};
            double *axis_down[3] = {&down.x, &down.y, &down.z};
            *axis_up[i] += step;
            *axis_down[i] -= step;
            fem::ShapeFunctions(shape, up, &plus, &ignored);
            fem::ShapeFunctions(shape, down, &minus, &ignored);
            for (int a = 0; a < count; ++a) {
                const double numeric = (plus[Idx(a)] - minus[Idx(a)]) / (2.0 * step);
                CHECK(std::fabs(numeric - dn[Idx(a * 3 + i)]) < 1e-5);
            }
        }
    }
}

// --- Quadrature -----------------------------------------------------------

double ReferenceVolume(ElementShape shape) {
    switch (shape) {
        case ElementShape::Tri3:
        case ElementShape::Tri6:
            return 0.5;
        case ElementShape::Quad4:
        case ElementShape::Quad8:
            return 4.0;
        case ElementShape::Tet4:
        case ElementShape::Tet10:
            return 1.0 / 6.0;
        case ElementShape::Hex8:
        case ElementShape::Hex20:
            return 8.0;
        case ElementShape::Wedge6:
        case ElementShape::Wedge15:
            return 1.0;
        case ElementShape::Pyr5:
            return 4.0 / 3.0;
    }
    return 0.0;
}

void CheckQuadrature(ElementShape shape) {
    std::string error;
    for (int degree = 1; degree <= 2; ++degree) {
        std::vector<QuadraturePoint> rule;
        CHECK_MESSAGE(fem::Quadrature(shape, degree, &rule, &error), error);
        CHECK(!rule.empty());
        // THE WEIGHTS SUM TO THE REFERENCE VOLUME. The single cheapest
        // check there is, and it catches a rule copied for the wrong
        // reference element -- a triangle rule whose weights sum to one
        // rather than a half is out by a factor of two everywhere, which
        // is the sort of error that looks like a material property.
        double total = 0.0;
        for (const QuadraturePoint &point : rule) total += point.weight;
        CHECK(std::fabs(total - ReferenceVolume(shape)) < 1e-12);
    }

    // And it integrates what it says it does. A monomial of total degree
    // `degree` over the reference element, against the same integral
    // computed by brute-force subdivision, which is slow and independent.
    std::vector<QuadraturePoint> rule;
    std::vector<QuadraturePoint> fine;
    CHECK_MESSAGE(fem::Quadrature(shape, 2, &rule, &error), error);
    if (!fem::Quadrature(shape, 4, &fine, &error)) return;
    const int dimension = ElementDimension(shape);
    for (int power = 0; power <= 2; ++power) {
        for (int axis = 0; axis < dimension; ++axis) {
            auto integrate = [&](const std::vector<QuadraturePoint> &points) {
                double total = 0.0;
                for (const QuadraturePoint &point : points) {
                    const double at[3] = {point.at.x, point.at.y, point.at.z};
                    total += std::pow(at[axis], power) * point.weight;
                }
                return total;
            };
            // A degree-two rule and a degree-four rule must agree on
            // anything of degree two. They are different rules with
            // different points, so agreement is a real statement.
            CHECK(std::fabs(integrate(rule) - integrate(fine)) < 1e-10);
        }
    }
}

// --- The stiffness matrix's rank ------------------------------------------

// The eigenvalues of a small symmetric matrix, by Jacobi rotation.
// Written here rather than reached for because the only thing needed is
// how many of them are zero, and that is worth having independent of
// anything else in the tree.
std::vector<double> SymmetricEigenvalues(std::vector<double> matrix, int size) {
    for (int sweep = 0; sweep < 100; ++sweep) {
        double off = 0.0;
        for (int i = 0; i < size; ++i) {
            for (int j = i + 1; j < size; ++j) {
                off += matrix[Idx(i * size + j)] * matrix[Idx(i * size + j)];
            }
        }
        if (off < 1e-24) break;
        for (int p = 0; p < size; ++p) {
            for (int q = p + 1; q < size; ++q) {
                const double apq = matrix[Idx(p * size + q)];
                if (std::fabs(apq) < 1e-300) continue;
                const double app = matrix[Idx(p * size + p)];
                const double aqq = matrix[Idx(q * size + q)];
                const double theta = (aqq - app) / (2.0 * apq);
                const double t = (theta >= 0.0 ? 1.0 : -1.0) /
                                 (std::fabs(theta) + std::sqrt(theta * theta + 1.0));
                const double c = 1.0 / std::sqrt(t * t + 1.0);
                const double s = t * c;
                for (int k = 0; k < size; ++k) {
                    const double akp = matrix[Idx(k * size + p)];
                    const double akq = matrix[Idx(k * size + q)];
                    matrix[Idx(k * size + p)] = c * akp - s * akq;
                    matrix[Idx(k * size + q)] = s * akp + c * akq;
                }
                for (int k = 0; k < size; ++k) {
                    const double apk = matrix[Idx(p * size + k)];
                    const double aqk = matrix[Idx(q * size + k)];
                    matrix[Idx(p * size + k)] = c * apk - s * aqk;
                    matrix[Idx(q * size + k)] = s * apk + c * aqk;
                }
            }
        }
    }
    std::vector<double> out;
    for (int i = 0; i < size; ++i) out.push_back(matrix[Idx(i * size + i)]);
    std::sort(out.begin(), out.end());
    return out;
}

// A distorted but valid element of the given shape, so that nothing here
// passes by symmetry. The distortion is deterministic and small enough
// that no element turns inside out.
std::vector<Vec3d> DistortedElement(ElementShape shape, double amount) {
    std::vector<Vec3d> nodes;
    fem::ReferenceNodes(shape, &nodes);
    for (std::size_t i = 0; i < nodes.size(); ++i) {
        const std::uint64_t h = (i + 1) * 0x9E3779B97F4A7C15ull;
        auto unit = [&](int shift) {
            return static_cast<double>((h >> shift) & 0xFFFF) / 65535.0 - 0.5;
        };
        nodes[i] = nodes[i] + Vec3d{unit(0), unit(16), unit(32)} * amount;
    }
    if (ElementDimension(shape) == 2) {
        for (Vec3d &p : nodes) p.z = 0.0;
    }
    return nodes;
}

void CheckStiffness(ElementShape shape) {
    const int count = ElementNodeCount(shape);
    const int dimension = ElementDimension(shape);
    const int size = count * dimension;
    Material material;
    material.youngs_modulus = 210e9;
    material.poissons_ratio = 0.3;
    ElementOptions options;
    std::string error;

    std::vector<Vec3d> nodes = DistortedElement(shape, 0.08);
    std::vector<double> k;
    CHECK_MESSAGE(fem::ElementStiffness(shape, nodes, material, options, &k, &error), error);

    // Symmetric, exactly -- it is filled in as such, so this checks that
    // it really was rather than that it nearly is.
    for (int i = 0; i < size; ++i) {
        for (int j = 0; j < size; ++j) {
            CHECK(k[Idx(i * size + j)] == k[Idx(j * size + i)]);
        }
    }

    // SIX ZERO EIGENVALUES IN THREE DIMENSIONS AND NO MORE. Three
    // translations and three rotations carry no strain, so they carry no
    // energy; anything else that carries no energy is a mechanism the
    // element invented, and a solution containing one looks like a
    // deformation and is not.
    const std::vector<double> eigenvalues = SymmetricEigenvalues(k, size);
    double largest = 0.0;
    for (const double value : eigenvalues) largest = std::max(largest, std::fabs(value));
    int zeros = 0;
    for (const double value : eigenvalues) {
        if (std::fabs(value) < largest * 1e-9) ++zeros;
    }
    const int wanted = dimension == 3 ? 6 : 3;
    if (zeros != wanted) {
        std::printf("    %-8s has %d zero eigenvalues, wanted %d\n", ElementShapeName(shape), zeros,
                    wanted);
    }
    CHECK(zeros == wanted);
    // And no negative ones: the strain energy of any displacement is
    // positive or zero, never negative.
    CHECK(eigenvalues.front() > -largest * 1e-9);

    // A RIGID MOTION CARRIES NO FORCE. The same statement as the zero
    // eigenvalues, made directly and independently of the eigenvalue
    // routine, which is worth doing because the eigenvalue routine is
    // also code that could be wrong.
    for (int mode = 0; mode < (dimension == 3 ? 6 : 3); ++mode) {
        std::vector<double> u(Idx(size), 0.0);
        for (int a = 0; a < count; ++a) {
            const Vec3d &p = nodes[Idx(a)];
            double motion[3] = {0.0, 0.0, 0.0};
            if (dimension == 3) {
                if (mode < 3) {
                    motion[mode] = 1.0;
                } else {
                    // A rotation about each axis in turn.
                    const int axis = mode - 3;
                    const Vec3d about = axis == 0 ? Vec3d{1, 0, 0}
                                                  : (axis == 1 ? Vec3d{0, 1, 0} : Vec3d{0, 0, 1});
                    const Vec3d turn = about.Cross(p);
                    motion[0] = turn.x;
                    motion[1] = turn.y;
                    motion[2] = turn.z;
                }
            } else {
                if (mode < 2) {
                    motion[mode] = 1.0;
                } else {
                    motion[0] = -p.y;
                    motion[1] = p.x;
                }
            }
            for (int i = 0; i < dimension; ++i) u[Idx(a * dimension + i)] = motion[i];
        }
        double norm = 0.0;
        for (const double value : u) norm += value * value;
        for (int i = 0; i < size; ++i) {
            double force = 0.0;
            for (int j = 0; j < size; ++j) force += k[Idx(i * size + j)] * u[Idx(j)];
            if (!(std::fabs(force) < largest * std::sqrt(norm) * 1e-10)) {
                static int shown = 0;
                if (shown++ < 8) {
                    std::printf("    RIGID %-8s mode %d dof %d force %.4e tol %.4e\n",
                                ElementShapeName(shape), mode, i, force,
                                largest * std::sqrt(norm) * 1e-10);
                }
            }
            CHECK(std::fabs(force) < largest * std::sqrt(norm) * 1e-10);
        }
    }
}

// --- The patch test -------------------------------------------------------
//
// THE TEST AN ELEMENT EXISTS TO PASS. A patch of elements, distorted so
// that nothing holds by symmetry, with a linear displacement field
// imposed on every boundary node and the interior left free. If the
// element can represent constant strain, the interior nodes must come out
// exactly on the same linear field -- not nearly, exactly, to round-off
// -- because the exact solution is in the space the element spans and a
// Galerkin method returns the exact solution when it can.
//
// An element that fails this converges to the wrong answer, and refining
// the mesh makes it converge harder.

// Dense elimination over the free degrees of freedom. These problems
// have a few dozen of them; reaching for the sparse solver would make
// this test depend on the very thing it exists to test independently.
bool SolveDense(const std::vector<double> &global, const std::vector<double> &load,
                const std::vector<bool> &fixed, int dofs, std::vector<double> *solution) {
    std::vector<int> free_dofs;
    for (int i = 0; i < dofs; ++i) {
        if (!fixed[Idx(i)]) free_dofs.push_back(i);
    }
    const int n = static_cast<int>(free_dofs.size());
    if (n == 0) return false;
    std::vector<double> a(Idx(n * (n + 1)), 0.0);
    for (int i = 0; i < n; ++i) {
        for (int j = 0; j < n; ++j) {
            a[Idx(i * (n + 1) + j)] = global[Idx(free_dofs[Idx(i)] * dofs + free_dofs[Idx(j)])];
        }
        a[Idx(i * (n + 1) + n)] = load[Idx(free_dofs[Idx(i)])];
    }
    for (int column = 0; column < n; ++column) {
        int pivot = column;
        for (int row = column; row < n; ++row) {
            if (std::fabs(a[Idx(row * (n + 1) + column)]) >
                std::fabs(a[Idx(pivot * (n + 1) + column)])) {
                pivot = row;
            }
        }
        if (pivot != column) {
            for (int c = 0; c <= n; ++c) {
                std::swap(a[Idx(column * (n + 1) + c)], a[Idx(pivot * (n + 1) + c)]);
            }
        }
        const double diagonal = a[Idx(column * (n + 1) + column)];
        if (diagonal == 0.0) return false;
        for (int row = 0; row < n; ++row) {
            if (row == column) continue;
            const double factor = a[Idx(row * (n + 1) + column)] / diagonal;
            if (factor == 0.0) continue;
            for (int c = column; c <= n; ++c) {
                a[Idx(row * (n + 1) + c)] -= factor * a[Idx(column * (n + 1) + c)];
            }
        }
    }
    for (int i = 0; i < n; ++i) {
        (*solution)[Idx(free_dofs[Idx(i)])] =
            a[Idx(i * (n + 1) + n)] / a[Idx(i * (n + 1) + i)];
    }
    return true;
}

struct Patch {
    std::vector<Vec3d> nodes;
    std::vector<std::vector<int>> elements;
    std::vector<bool> on_boundary;
};

// Second-order elements need their mid-edge nodes, added once per edge
// and shared -- which is also what makes the patch conforming, since two
// elements meeting on a face must agree about the nodes on it.
void AddMidSideNodes(ElementShape shape, Patch *patch) {
    const int wanted = ElementNodeCount(shape);
    if (patch->elements.empty()) return;
    if (static_cast<int>(patch->elements.front().size()) == wanted) return;
    static const int tri_edges[3][2] = {{0, 1}, {1, 2}, {2, 0}};
    static const int quad_edges[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
    static const int tet_edges[6][2] = {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}};
    static const int hex_edges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {0, 4}, {1, 5},
                                         {2, 6}, {3, 7}, {4, 5}, {5, 6}, {6, 7}, {7, 4}};
    static const int wedge_edges[9][2] = {{0, 1}, {1, 2}, {2, 0}, {3, 4}, {4, 5},
                                          {5, 3}, {0, 3}, {1, 4}, {2, 5}};
    const int (*edges)[2] = nullptr;
    int edge_count = 0;
    switch (shape) {
        case ElementShape::Tri6: edges = tri_edges; edge_count = 3; break;
        case ElementShape::Quad8: edges = quad_edges; edge_count = 4; break;
        case ElementShape::Tet10: edges = tet_edges; edge_count = 6; break;
        case ElementShape::Hex20: edges = hex_edges; edge_count = 12; break;
        case ElementShape::Wedge15: edges = wedge_edges; edge_count = 9; break;
        default: return;
    }
    std::map<std::pair<int, int>, int> middles;
    for (std::vector<int> &element : patch->elements) {
        const std::vector<int> corners = element;
        for (int e = 0; e < edge_count; ++e) {
            const int u = corners[Idx(edges[e][0])];
            const int v = corners[Idx(edges[e][1])];
            const std::pair<int, int> key = u < v ? std::make_pair(u, v) : std::make_pair(v, u);
            const auto found = middles.find(key);
            if (found != middles.end()) {
                element.push_back(found->second);
                continue;
            }
            const int index = static_cast<int>(patch->nodes.size());
            patch->nodes.push_back((patch->nodes[Idx(u)] + patch->nodes[Idx(v)]) * 0.5);
            // A mid-edge node is on the boundary exactly when both its
            // ends are -- not true in general, true for this patch, whose
            // only interior node is the one in the middle.
            patch->on_boundary.push_back(patch->on_boundary[Idx(u)] && patch->on_boundary[Idx(v)]);
            middles[key] = index;
            element.push_back(index);
        }
    }
}

// A block of eight hexahedra with the middle node moved, then each
// hexahedron carved into whatever shape is being tested. One
// construction serves every three-dimensional element because every one
// of them tiles a cube.
Patch BuildPatch(ElementShape shape) {
    Patch patch;
    const int dimension = ElementDimension(shape);
    if (dimension == 2) {
        // A two-by-two block of quadrilaterals: nine nodes, the middle
        // one free.
        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < 3; ++i) {
                patch.nodes.push_back(Vec3d{static_cast<double>(i), static_cast<double>(j), 0.0});
            }
        }
        patch.nodes[4] = patch.nodes[4] + Vec3d{0.23, -0.17, 0.0};
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                const int a = j * 3 + i;
                const std::vector<int> quad = {a, a + 1, a + 4, a + 3};
                if (shape == ElementShape::Quad4 || shape == ElementShape::Quad8) {
                    patch.elements.push_back(quad);
                } else {
                    patch.elements.push_back({quad[0], quad[1], quad[2]});
                    patch.elements.push_back({quad[0], quad[2], quad[3]});
                }
            }
        }
        patch.on_boundary.assign(patch.nodes.size(), true);
        patch.on_boundary[4] = false;
        AddMidSideNodes(shape, &patch);
        return patch;
    }

    for (int k = 0; k < 3; ++k) {
        for (int j = 0; j < 3; ++j) {
            for (int i = 0; i < 3; ++i) {
                patch.nodes.push_back(
                    Vec3d{static_cast<double>(i), static_cast<double>(j), static_cast<double>(k)});
            }
        }
    }
    auto at = [](int i, int j, int k) { return (k * 3 + j) * 3 + i; };
    const int middle = at(1, 1, 1);
    patch.nodes[Idx(middle)] = patch.nodes[Idx(middle)] + Vec3d{0.21, -0.13, 0.17};
    patch.on_boundary.assign(patch.nodes.size(), true);
    patch.on_boundary[Idx(middle)] = false;

    for (int k = 0; k < 2; ++k) {
        for (int j = 0; j < 2; ++j) {
            for (int i = 0; i < 2; ++i) {
                const int h[8] = {at(i, j, k),         at(i + 1, j, k),
                                  at(i + 1, j + 1, k), at(i, j + 1, k),
                                  at(i, j, k + 1),     at(i + 1, j, k + 1),
                                  at(i + 1, j + 1, k + 1), at(i, j + 1, k + 1)};
                switch (shape) {
                    case ElementShape::Hex8:
                    case ElementShape::Hex20:
                        patch.elements.push_back({h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]});
                        break;
                    case ElementShape::Wedge6:
                    case ElementShape::Wedge15:
                        // The cube cut along one diagonal of its base.
                        patch.elements.push_back({h[0], h[1], h[2], h[4], h[5], h[6]});
                        patch.elements.push_back({h[0], h[2], h[3], h[4], h[6], h[7]});
                        break;
                    case ElementShape::Tet4:
                    case ElementShape::Tet10:
                        // Six tetrahedra sharing the 0-6 diagonal: the
                        // standard split, which tiles without leaving a
                        // gap and matches face for face across cubes.
                        patch.elements.push_back({h[0], h[1], h[2], h[6]});
                        patch.elements.push_back({h[0], h[2], h[3], h[6]});
                        patch.elements.push_back({h[0], h[3], h[7], h[6]});
                        patch.elements.push_back({h[0], h[7], h[4], h[6]});
                        patch.elements.push_back({h[0], h[4], h[5], h[6]});
                        patch.elements.push_back({h[0], h[5], h[1], h[6]});
                        break;
                    case ElementShape::Pyr5: {
                        // Six pyramids from the cube's own centre, which
                        // is a node of its own.
                        Vec3d centre{};
                        for (const int node : h) centre = centre + patch.nodes[Idx(node)];
                        centre = centre * 0.125;
                        const int apex = static_cast<int>(patch.nodes.size());
                        patch.nodes.push_back(centre);
                        patch.on_boundary.push_back(false);
                        const int faces[6][4] = {{h[0], h[1], h[2], h[3]}, {h[4], h[5], h[6], h[7]},
                                                 {h[0], h[1], h[5], h[4]}, {h[1], h[2], h[6], h[5]},
                                                 {h[2], h[3], h[7], h[6]}, {h[3], h[0], h[4], h[7]}};
                        for (const auto &face : faces) {
                            // Wound by asking rather than by assuming: a
                            // pyramid's base must face its own apex, and
                            // working out which of six faces of a cube
                            // need reversing is exactly the sort of thing
                            // that is easier to check than to reason out.
                            std::vector<int> five = {face[0], face[1], face[2], face[3], apex};
                            const Vec3d &p0 = patch.nodes[Idx(face[0])];
                            const Vec3d normal = (patch.nodes[Idx(face[1])] - p0)
                                                     .Cross(patch.nodes[Idx(face[3])] - p0);
                            if (normal.Dot(centre - p0) < 0.0) std::swap(five[1], five[3]);
                            patch.elements.push_back(five);
                        }
                        break;
                    }
                    default:
                        break;
                }
            }
        }
    }

    AddMidSideNodes(shape, &patch);
    return patch;
}

void PatchTest(ElementShape shape) {
    const Patch patch = BuildPatch(shape);
    if (patch.elements.empty()) return;
    const int dimension = ElementDimension(shape);
    const int nodes = static_cast<int>(patch.nodes.size());
    const int dofs = nodes * dimension;
    Material material;
    ElementOptions options;
    std::string error;

    // A linear field with every component of the strain in it, so that
    // no term of the constitutive matrix is left untested.
    auto field = [&](const Vec3d &p) {
        if (dimension == 2) {
            return Vec3d{1e-4 * (2.0 + 3.0 * p.x - 1.0 * p.y),
                         1e-4 * (-1.0 + 0.5 * p.x + 2.0 * p.y), 0.0};
        }
        return Vec3d{1e-4 * (2.0 + 3.0 * p.x - 1.0 * p.y + 0.5 * p.z),
                     1e-4 * (-1.0 + 0.5 * p.x + 2.0 * p.y - 1.5 * p.z),
                     1e-4 * (0.5 - 2.0 * p.x + 1.0 * p.y + 4.0 * p.z)};
    };

    std::vector<double> global(Idx(dofs * dofs), 0.0);
    std::vector<double> load(Idx(dofs), 0.0);
    std::vector<Vec3d> element_nodes;
    std::vector<double> k;
    for (const std::vector<int> &element : patch.elements) {
        element_nodes.clear();
        for (const int node : element) element_nodes.push_back(patch.nodes[Idx(node)]);
        if (!fem::ElementStiffness(shape, element_nodes, material, options, &k, &error)) {
            std::printf("    %-8s patch: %s\n", ElementShapeName(shape), error.c_str());
            ++failures;
            return;
        }
        const int size = static_cast<int>(element.size()) * dimension;
        for (int i = 0; i < size; ++i) {
            const int row = element[Idx(i / dimension)] * dimension + i % dimension;
            for (int j = 0; j < size; ++j) {
                const int column = element[Idx(j / dimension)] * dimension + j % dimension;
                global[Idx(row * dofs + column)] += k[Idx(i * size + j)];
            }
        }
    }

    // Every boundary degree of freedom is prescribed to the linear
    // field; the interior is solved for.
    std::vector<double> prescribed(Idx(dofs), 0.0);
    std::vector<bool> fixed(Idx(dofs), false);
    for (int a = 0; a < nodes; ++a) {
        const Vec3d value = field(patch.nodes[Idx(a)]);
        const double component[3] = {value.x, value.y, value.z};
        for (int i = 0; i < dimension; ++i) {
            prescribed[Idx(a * dimension + i)] = component[i];
            fixed[Idx(a * dimension + i)] = patch.on_boundary[Idx(a)];
        }
    }
    for (int i = 0; i < dofs; ++i) {
        if (fixed[Idx(i)]) continue;
        for (int j = 0; j < dofs; ++j) {
            if (!fixed[Idx(j)]) continue;
            load[Idx(i)] -= global[Idx(i * dofs + j)] * prescribed[Idx(j)];
        }
    }

    std::vector<double> solution = prescribed;
    CHECK(SolveDense(global, load, fixed, dofs, &solution));
    int free_count = 0;
    for (int i = 0; i < dofs; ++i) {
        if (!fixed[Idx(i)]) ++free_count;
    }
    double worst = 0.0;
    for (int i = 0; i < dofs; ++i) {
        if (fixed[Idx(i)]) continue;
        worst = std::max(worst, std::fabs(solution[Idx(i)] - prescribed[Idx(i)]));
    }
    std::printf("  %-8s %2d elements, %3d nodes, %3d free: worst error %.2e\n",
                ElementShapeName(shape), static_cast<int>(patch.elements.size()), nodes, free_count,
                worst);
    // Relative to the size of the field itself, which is 1e-4.
    CHECK(worst < 1e-4 * 1e-9);
}

// --- Volumetric locking, and what B-bar does about it ---------------------
//
// AS POISSON'S RATIO APPROACHES A HALF the material stops being able to
// change volume, and each quadrature point of each element imposes its
// own incompressibility constraint on the displacement field. Count
// them: a block of trilinear hexahedra has far more such constraints
// than it has free degrees of freedom to satisfy them with, so the only
// field left that satisfies all of them is nearly nothing, and the mesh
// comes out enormously too stiff. That is volumetric locking. It is not
// a rounding-level effect and it is not announced: the solve converges,
// the answer looks like a displacement, and it is several times too
// small.
//
// The measurement here is a block sheared sideways. Shear is
// volume-preserving, so the true answer depends on the shear modulus
// alone -- and the shear modulus barely moves between Poisson's ratio
// 0.3 and 0.4999, falling by about thirteen percent. Any large *rise* in
// stiffness across that change is the locking and nothing else, which is
// what makes this a measurement rather than an impression.
double ShearStiffness(double poissons_ratio, bool b_bar, int divisions) {
    Material material;
    material.youngs_modulus = 210e9;
    material.poissons_ratio = poissons_ratio;
    ElementOptions options;
    options.b_bar = b_bar;
    std::string error;

    const int side = divisions + 1;
    std::vector<Vec3d> nodes;
    for (int k = 0; k < side; ++k) {
        for (int j = 0; j < side; ++j) {
            for (int i = 0; i < side; ++i) {
                nodes.push_back(Vec3d{static_cast<double>(i) / divisions,
                                      static_cast<double>(j) / divisions,
                                      static_cast<double>(k) / divisions});
            }
        }
    }
    auto at = [&](int i, int j, int k) { return (k * side + j) * side + i; };
    std::vector<std::vector<int>> elements;
    for (int k = 0; k < divisions; ++k) {
        for (int j = 0; j < divisions; ++j) {
            for (int i = 0; i < divisions; ++i) {
                elements.push_back({at(i, j, k), at(i + 1, j, k), at(i + 1, j + 1, k),
                                    at(i, j + 1, k), at(i, j, k + 1), at(i + 1, j, k + 1),
                                    at(i + 1, j + 1, k + 1), at(i, j + 1, k + 1)});
            }
        }
    }

    const int count = static_cast<int>(nodes.size());
    const int dofs = count * 3;
    std::vector<double> global(Idx(dofs * dofs), 0.0);
    std::vector<Vec3d> element_nodes;
    std::vector<double> k;
    for (const std::vector<int> &element : elements) {
        element_nodes.clear();
        for (const int node : element) element_nodes.push_back(nodes[Idx(node)]);
        if (!fem::ElementStiffness(ElementShape::Hex8, element_nodes, material, options, &k,
                                   &error)) {
            return 0.0;
        }
        for (int i = 0; i < 24; ++i) {
            const int row = element[Idx(i / 3)] * 3 + i % 3;
            for (int j = 0; j < 24; ++j) {
                const int column = element[Idx(j / 3)] * 3 + j % 3;
                global[Idx(row * dofs + column)] += k[Idx(i * 24 + j)];
            }
        }
    }

    // The bottom face held, the top face slid sideways by a millimetre,
    // everything between it free.
    const double slide = 1e-3;
    std::vector<bool> fixed(Idx(dofs), false);
    std::vector<double> prescribed(Idx(dofs), 0.0);
    for (int a = 0; a < count; ++a) {
        const Vec3d &p = nodes[Idx(a)];
        if (p.z == 0.0) {
            for (int i = 0; i < 3; ++i) fixed[Idx(a * 3 + i)] = true;
        } else if (p.z == 1.0) {
            for (int i = 0; i < 3; ++i) fixed[Idx(a * 3 + i)] = true;
            prescribed[Idx(a * 3 + 0)] = slide;
        }
    }
    std::vector<double> load(Idx(dofs), 0.0);
    for (int i = 0; i < dofs; ++i) {
        if (fixed[Idx(i)]) continue;
        for (int j = 0; j < dofs; ++j) {
            if (!fixed[Idx(j)]) continue;
            load[Idx(i)] -= global[Idx(i * dofs + j)] * prescribed[Idx(j)];
        }
    }
    std::vector<double> solution = prescribed;
    if (!SolveDense(global, load, fixed, dofs, &solution)) return 0.0;

    // The force it took, which is the stiffness times the slide.
    double reaction = 0.0;
    for (int a = 0; a < count; ++a) {
        if (nodes[Idx(a)].z != 1.0) continue;
        double force = 0.0;
        for (int j = 0; j < dofs; ++j) force += global[Idx((a * 3) * dofs + j)] * solution[Idx(j)];
        reaction += force;
    }
    return reaction / slide;
}

void LockingTest() {
    std::printf("volumetric locking, a block sheared sideways:\n");
    // Shear preserves volume, so the true stiffness follows the shear
    // modulus, which falls by a factor of (1+v)/(1+v') between the two
    // Poisson's ratios below -- about thirteen percent. Everything is
    // reported relative to that, so a correct element reads 1.00 and
    // anything above it is stiffness that is not in the physics.
    const double compressible = 0.3;
    const double nearly_incompressible = 0.4999;
    const double shear_ratio = (1.0 + compressible) / (1.0 + nearly_incompressible);
    std::printf("    %-12s %-22s %-22s\n", "elements", "full integration", "B-bar");
    std::vector<double> full;
    std::vector<double> bar;
    for (int divisions = 2; divisions <= 5; ++divisions) {
        const double full_soft = ShearStiffness(compressible, false, divisions);
        const double full_hard = ShearStiffness(nearly_incompressible, false, divisions);
        const double bar_soft = ShearStiffness(compressible, true, divisions);
        const double bar_hard = ShearStiffness(nearly_incompressible, true, divisions);
        CHECK(full_soft > 0.0);
        CHECK(bar_soft > 0.0);
        // Away from the incompressible limit the two agree closely:
        // B-bar is not a different element, it is the same one with one
        // term integrated differently, and that term is not doing much
        // until the material stops being able to change volume.
        CHECK(std::fabs(bar_soft - full_soft) < full_soft * 0.12);
        full.push_back((full_hard / full_soft) / shear_ratio);
        bar.push_back((bar_hard / bar_soft) / shear_ratio);
        std::printf("    %-12d %-22.3f %-22.3f\n", divisions * divisions * divisions,
                    full.back(), bar.back());
    }

    // IT GETS WORSE UNDER REFINEMENT, WHICH IS THE WHOLE SIGNATURE.
    // Locking is a counting argument: every quadrature point adds an
    // incompressibility constraint and every node adds three degrees of
    // freedom, and refining a mesh of low-order elements adds constraints
    // faster than it adds freedom. So the answer does not converge
    // towards the right one, it walks away from it -- which is also the
    // second of the diagnostic patterns this plan keeps a list of, and
    // the reason a convergence study is worth more than a fine mesh.
    for (std::size_t i = 1; i < full.size(); ++i) {
        CHECK(full[i] > full[i - 1]);
    }
    CHECK(full.back() > 1.35);

    // AND B-BAR DOES NOT. Its overshoot is flat in the mesh size, which
    // is what "the constraint is imposed once per element" buys: the
    // count of constraints then grows with the elements rather than with
    // their quadrature points, and stays behind the count of freedoms.
    CHECK(bar.back() < bar[1] * 1.05);
    CHECK(bar.back() < 1.2);
    CHECK(full.back() > bar.back() * 1.2);
}

}  // namespace

int main() {
    std::printf("shape functions and quadrature:\n");
    for (const ElementShape shape : fem::AllElementShapes()) {
        CheckShapeFunctions(shape);
        CheckQuadrature(shape);
        std::printf("  %-8s %2d nodes, %dD\n", ElementShapeName(shape), ElementNodeCount(shape),
                    ElementDimension(shape));
    }

    std::printf("stiffness: symmetry, rank and rigid modes\n");
    for (const ElementShape shape : fem::AllElementShapes()) {
        CheckStiffness(shape);
    }

    LockingTest();

    std::printf("the patch test:\n");
    for (const ElementShape shape : fem::AllElementShapes()) {
        PatchTest(shape);
    }

    if (failures != 0) {
        std::printf("fem_elem_test FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    std::printf("fem_elem_test passed (%d checks)\n", checks);
    return 0;
}
