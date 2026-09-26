#include "fem_elem.h"

#include "fem_solve.h"

#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace fem {
namespace {

using cad::Vec3d;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// --- Reference node positions ---------------------------------------------
//
// THE ORDERINGS, ONCE, EXPLICITLY, because every one of them is a
// convention that some other program shares and getting one wrong
// produces a mesh that solves to nonsense rather than one that fails.
// These are Abaqus's and VTK's, which agree for all of these: corners
// first in a consistent rotational sense, then mid-edge nodes in a
// defined edge order.
const double kTri3[3][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}};
const double kQuad4[4][3] = {{-1, -1, 0}, {1, -1, 0}, {1, 1, 0}, {-1, 1, 0}};
const double kTet4[4][3] = {{0, 0, 0}, {1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
const double kHex8[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                            {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};
// Edges, in the order the mid-side nodes follow the corners.
const int kTriEdges[3][2] = {{0, 1}, {1, 2}, {2, 0}};
const int kQuadEdges[4][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}};
const int kTetEdges[6][2] = {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}};
const int kHexEdges[12][2] = {{0, 1}, {1, 2}, {2, 3}, {3, 0}, {0, 4}, {1, 5},
                              {2, 6}, {3, 7}, {4, 5}, {5, 6}, {6, 7}, {7, 4}};
const int kWedgeEdges[9][2] = {{0, 1}, {1, 2}, {2, 0}, {3, 4}, {4, 5},
                               {5, 3}, {0, 3}, {1, 4}, {2, 5}};

// --- One-dimensional Gauss-Legendre ---------------------------------------
bool GaussLegendre(int points, std::vector<double> *at, std::vector<double> *weight) {
    at->clear();
    weight->clear();
    switch (points) {
        case 1:
            *at = {0.0};
            *weight = {2.0};
            return true;
        case 2:
            *at = {-0.57735026918962576451, 0.57735026918962576451};
            *weight = {1.0, 1.0};
            return true;
        case 3:
            *at = {-0.77459666924148337704, 0.0, 0.77459666924148337704};
            *weight = {0.55555555555555555556, 0.88888888888888888889, 0.55555555555555555556};
            return true;
        case 4:
            *at = {-0.86113631159405257522, -0.33998104358485626480, 0.33998104358485626480,
                   0.86113631159405257522};
            *weight = {0.34785484513745385737, 0.65214515486254614263, 0.65214515486254614263,
                       0.34785484513745385737};
            return true;
        case 5:
            *at = {-0.90617984593866399280, -0.53846931010568309104, 0.0,
                   0.53846931010568309104, 0.90617984593866399280};
            *weight = {0.23692688505618908751, 0.47862867049936646804, 0.56888888888888888889,
                       0.47862867049936646804, 0.23692688505618908751};
            return true;
        default:
            return false;
    }
}

// The number of Gauss points needed for a given polynomial degree: a
// rule with n points is exact to degree 2n-1.
int PointsForDegree(int degree) { return std::max(1, (degree + 2) / 2); }

// --- Triangle rules --------------------------------------------------------
//
// Symmetric rules over the reference triangle, whose area is 1/2, so the
// weights sum to 1/2 rather than to 1.
bool TriangleRule(int degree, std::vector<QuadraturePoint> *out) {
    out->clear();
    if (degree <= 1) {
        out->push_back({Vec3d{1.0 / 3.0, 1.0 / 3.0, 0.0}, 0.5});
        return true;
    }
    if (degree <= 2) {
        const double a = 1.0 / 6.0;
        const double b = 2.0 / 3.0;
        out->push_back({Vec3d{a, a, 0.0}, 1.0 / 6.0});
        out->push_back({Vec3d{b, a, 0.0}, 1.0 / 6.0});
        out->push_back({Vec3d{a, b, 0.0}, 1.0 / 6.0});
        return true;
    }
    if (degree <= 4) {
        // The six-point degree-four rule: two orbits of three.
        const double a1 = 0.44594849091596488632;
        const double w1 = 0.22338158967801146570 * 0.5;
        const double a2 = 0.09157621350977074346;
        const double w2 = 0.10995174365532186764 * 0.5;
        const double b1 = 1.0 - 2.0 * a1;
        const double b2 = 1.0 - 2.0 * a2;
        out->push_back({Vec3d{a1, a1, 0.0}, w1});
        out->push_back({Vec3d{b1, a1, 0.0}, w1});
        out->push_back({Vec3d{a1, b1, 0.0}, w1});
        out->push_back({Vec3d{a2, a2, 0.0}, w2});
        out->push_back({Vec3d{b2, a2, 0.0}, w2});
        out->push_back({Vec3d{a2, b2, 0.0}, w2});
        return true;
    }
    return false;
}

// --- Tetrahedron rules -----------------------------------------------------
//
// Over the reference tetrahedron, whose volume is 1/6.
bool TetrahedronRule(int degree, std::vector<QuadraturePoint> *out) {
    out->clear();
    if (degree <= 1) {
        out->push_back({Vec3d{0.25, 0.25, 0.25}, 1.0 / 6.0});
        return true;
    }
    if (degree <= 2) {
        const double a = 0.13819660112501051518;  // (5 - sqrt(5))/20
        const double b = 0.58541019662496845446;  // (5 + 3*sqrt(5))/20
        const double w = 1.0 / 24.0;
        out->push_back({Vec3d{a, a, a}, w});
        out->push_back({Vec3d{b, a, a}, w});
        out->push_back({Vec3d{a, b, a}, w});
        out->push_back({Vec3d{a, a, b}, w});
        return true;
    }
    if (degree <= 3) {
        // Five points, and one of the weights is negative -- which is
        // correct for this rule and not a typo. A negative weight cannot
        // make a stiffness matrix indefinite on its own, but it can make
        // a *mass* matrix so, which is why the degree is asked for
        // explicitly rather than chosen generously.
        out->push_back({Vec3d{0.25, 0.25, 0.25}, -4.0 / 30.0});
        const double a = 1.0 / 6.0;
        const double b = 0.5;
        const double w = 9.0 / 120.0;
        out->push_back({Vec3d{a, a, a}, w});
        out->push_back({Vec3d{b, a, a}, w});
        out->push_back({Vec3d{a, b, a}, w});
        out->push_back({Vec3d{a, a, b}, w});
        return true;
    }
    if (degree <= 4) {
        // Keast's eleven-point rule, degree four, all weights positive.
        out->push_back({Vec3d{0.25, 0.25, 0.25}, -0.01315555555555555556 * 6.0 / 6.0});
        const double a = 0.0714285714285714285;
        const double b = 0.785714285714285714;
        const double wa = 0.00762222222222222222;
        out->push_back({Vec3d{a, a, a}, wa});
        out->push_back({Vec3d{b, a, a}, wa});
        out->push_back({Vec3d{a, b, a}, wa});
        out->push_back({Vec3d{a, a, b}, wa});
        const double c = 0.399403576166799219;
        const double d = 0.100596423833200785;
        const double wc = 0.02488888888888888889;
        out->push_back({Vec3d{c, c, d}, wc});
        out->push_back({Vec3d{c, d, c}, wc});
        out->push_back({Vec3d{d, c, c}, wc});
        out->push_back({Vec3d{d, d, c}, wc});
        out->push_back({Vec3d{d, c, d}, wc});
        out->push_back({Vec3d{c, d, d}, wc});
        return true;
    }
    return false;
}

}  // namespace

const std::vector<ElementShape> &AllElementShapes() {
    static const std::vector<ElementShape> all = {
        ElementShape::Tri3,    ElementShape::Tri6,    ElementShape::Quad4,
        ElementShape::Quad8,   ElementShape::Tet4,    ElementShape::Tet10,
        ElementShape::Hex8,    ElementShape::Hex20,   ElementShape::Wedge6,
        ElementShape::Wedge15, ElementShape::Pyr5};
    return all;
}

const char *ElementShapeName(ElementShape shape) {
    switch (shape) {
        case ElementShape::Tri3: return "Tri3";
        case ElementShape::Tri6: return "Tri6";
        case ElementShape::Quad4: return "Quad4";
        case ElementShape::Quad8: return "Quad8";
        case ElementShape::Tet4: return "Tet4";
        case ElementShape::Tet10: return "Tet10";
        case ElementShape::Hex8: return "Hex8";
        case ElementShape::Hex20: return "Hex20";
        case ElementShape::Wedge6: return "Wedge6";
        case ElementShape::Wedge15: return "Wedge15";
        case ElementShape::Pyr5: return "Pyr5";
    }
    return "?";
}

int ElementNodeCount(ElementShape shape) {
    switch (shape) {
        case ElementShape::Tri3: return 3;
        case ElementShape::Tri6: return 6;
        case ElementShape::Quad4: return 4;
        case ElementShape::Quad8: return 8;
        case ElementShape::Tet4: return 4;
        case ElementShape::Tet10: return 10;
        case ElementShape::Hex8: return 8;
        case ElementShape::Hex20: return 20;
        case ElementShape::Wedge6: return 6;
        case ElementShape::Wedge15: return 15;
        case ElementShape::Pyr5: return 5;
    }
    return 0;
}

int ElementDimension(ElementShape shape) {
    switch (shape) {
        case ElementShape::Tri3:
        case ElementShape::Tri6:
        case ElementShape::Quad4:
        case ElementShape::Quad8:
            return 2;
        default:
            return 3;
    }
}

void ReferenceNodes(ElementShape shape, std::vector<Vec3d> *out) {
    out->clear();
    auto corners = [&](const double (*table)[3], int count) {
        for (int i = 0; i < count; ++i) {
            out->push_back(Vec3d{table[i][0], table[i][1], table[i][2]});
        }
    };
    auto middles = [&](const int (*edges)[2], int count) {
        for (int i = 0; i < count; ++i) {
            out->push_back(((*out)[Idx(edges[i][0])] + (*out)[Idx(edges[i][1])]) * 0.5);
        }
    };
    switch (shape) {
        case ElementShape::Tri3: corners(kTri3, 3); return;
        case ElementShape::Tri6: corners(kTri3, 3); middles(kTriEdges, 3); return;
        case ElementShape::Quad4: corners(kQuad4, 4); return;
        case ElementShape::Quad8: corners(kQuad4, 4); middles(kQuadEdges, 4); return;
        case ElementShape::Tet4: corners(kTet4, 4); return;
        case ElementShape::Tet10: corners(kTet4, 4); middles(kTetEdges, 6); return;
        case ElementShape::Hex8: corners(kHex8, 8); return;
        case ElementShape::Hex20: corners(kHex8, 8); middles(kHexEdges, 12); return;
        case ElementShape::Wedge6:
        case ElementShape::Wedge15:
            for (int side = 0; side < 2; ++side) {
                const double z = side == 0 ? -1.0 : 1.0;
                for (int i = 0; i < 3; ++i) {
                    out->push_back(Vec3d{kTri3[i][0], kTri3[i][1], z});
                }
            }
            if (shape == ElementShape::Wedge15) middles(kWedgeEdges, 9);
            return;
        case ElementShape::Pyr5:
            for (int i = 0; i < 4; ++i) {
                out->push_back(Vec3d{kQuad4[i][0], kQuad4[i][1], 0.0});
            }
            out->push_back(Vec3d{0.0, 0.0, 1.0});
            return;
    }
}

void ShapeFunctions(ElementShape shape, const Vec3d &at, std::vector<double> *out_n,
                    std::vector<double> *out_dn) {
    const int count = ElementNodeCount(shape);
    out_n->assign(Idx(count), 0.0);
    out_dn->assign(Idx(count * 3), 0.0);
    double *n = out_n->data();
    double *d = out_dn->data();
    const double r = at.x;
    const double s = at.y;
    const double t = at.z;
    auto set = [&](int i, double value, double dr, double ds, double dt) {
        n[Idx(i)] = value;
        d[Idx(i * 3 + 0)] = dr;
        d[Idx(i * 3 + 1)] = ds;
        d[Idx(i * 3 + 2)] = dt;
    };

    switch (shape) {
        case ElementShape::Tri3: {
            set(0, 1.0 - r - s, -1.0, -1.0, 0.0);
            set(1, r, 1.0, 0.0, 0.0);
            set(2, s, 0.0, 1.0, 0.0);
            return;
        }
        case ElementShape::Tri6: {
            const double area[3] = {1.0 - r - s, r, s};
            const double da[3][2] = {{-1.0, -1.0}, {1.0, 0.0}, {0.0, 1.0}};
            for (int i = 0; i < 3; ++i) {
                set(i, area[i] * (2.0 * area[i] - 1.0), (4.0 * area[i] - 1.0) * da[i][0],
                    (4.0 * area[i] - 1.0) * da[i][1], 0.0);
            }
            for (int e = 0; e < 3; ++e) {
                const int i = kTriEdges[e][0];
                const int j = kTriEdges[e][1];
                set(3 + e, 4.0 * area[i] * area[j],
                    4.0 * (da[i][0] * area[j] + area[i] * da[j][0]),
                    4.0 * (da[i][1] * area[j] + area[i] * da[j][1]), 0.0);
            }
            return;
        }
        case ElementShape::Quad4: {
            for (int i = 0; i < 4; ++i) {
                const double a = kQuad4[i][0];
                const double b = kQuad4[i][1];
                set(i, 0.25 * (1.0 + a * r) * (1.0 + b * s), 0.25 * a * (1.0 + b * s),
                    0.25 * b * (1.0 + a * r), 0.0);
            }
            return;
        }
        case ElementShape::Quad8: {
            for (int i = 0; i < 4; ++i) {
                const double a = kQuad4[i][0];
                const double b = kQuad4[i][1];
                const double p = 1.0 + a * r;
                const double q = 1.0 + b * s;
                set(i, 0.25 * p * q * (a * r + b * s - 1.0),
                    0.25 * a * q * (2.0 * a * r + b * s), 0.25 * b * p * (a * r + 2.0 * b * s),
                    0.0);
            }
            for (int e = 0; e < 4; ++e) {
                const int i = kQuadEdges[e][0];
                const int j = kQuadEdges[e][1];
                const double a = (kQuad4[i][0] + kQuad4[j][0]) * 0.5;
                const double b = (kQuad4[i][1] + kQuad4[j][1]) * 0.5;
                if (a == 0.0) {
                    set(4 + e, 0.5 * (1.0 - r * r) * (1.0 + b * s), -r * (1.0 + b * s),
                        0.5 * b * (1.0 - r * r), 0.0);
                } else {
                    set(4 + e, 0.5 * (1.0 + a * r) * (1.0 - s * s), 0.5 * a * (1.0 - s * s),
                        -s * (1.0 + a * r), 0.0);
                }
            }
            return;
        }
        case ElementShape::Tet4: {
            set(0, 1.0 - r - s - t, -1.0, -1.0, -1.0);
            set(1, r, 1.0, 0.0, 0.0);
            set(2, s, 0.0, 1.0, 0.0);
            set(3, t, 0.0, 0.0, 1.0);
            return;
        }
        case ElementShape::Tet10: {
            const double vol[4] = {1.0 - r - s - t, r, s, t};
            const double dv[4][3] = {
                {-1.0, -1.0, -1.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
            for (int i = 0; i < 4; ++i) {
                set(i, vol[i] * (2.0 * vol[i] - 1.0), (4.0 * vol[i] - 1.0) * dv[i][0],
                    (4.0 * vol[i] - 1.0) * dv[i][1], (4.0 * vol[i] - 1.0) * dv[i][2]);
            }
            for (int e = 0; e < 6; ++e) {
                const int i = kTetEdges[e][0];
                const int j = kTetEdges[e][1];
                set(4 + e, 4.0 * vol[i] * vol[j],
                    4.0 * (dv[i][0] * vol[j] + vol[i] * dv[j][0]),
                    4.0 * (dv[i][1] * vol[j] + vol[i] * dv[j][1]),
                    4.0 * (dv[i][2] * vol[j] + vol[i] * dv[j][2]));
            }
            return;
        }
        case ElementShape::Hex8: {
            for (int i = 0; i < 8; ++i) {
                const double a = kHex8[i][0];
                const double b = kHex8[i][1];
                const double c = kHex8[i][2];
                set(i, 0.125 * (1.0 + a * r) * (1.0 + b * s) * (1.0 + c * t),
                    0.125 * a * (1.0 + b * s) * (1.0 + c * t),
                    0.125 * b * (1.0 + a * r) * (1.0 + c * t),
                    0.125 * c * (1.0 + a * r) * (1.0 + b * s));
            }
            return;
        }
        case ElementShape::Hex20: {
            for (int i = 0; i < 8; ++i) {
                const double a = kHex8[i][0];
                const double b = kHex8[i][1];
                const double c = kHex8[i][2];
                const double p = 1.0 + a * r;
                const double q = 1.0 + b * s;
                const double w = 1.0 + c * t;
                const double f = a * r + b * s + c * t - 2.0;
                set(i, 0.125 * p * q * w * f, 0.125 * a * q * w * (f + p),
                    0.125 * b * p * w * (f + q), 0.125 * c * p * q * (f + w));
            }
            for (int e = 0; e < 12; ++e) {
                const int i = kHexEdges[e][0];
                const int j = kHexEdges[e][1];
                const double a = (kHex8[i][0] + kHex8[j][0]) * 0.5;
                const double b = (kHex8[i][1] + kHex8[j][1]) * 0.5;
                const double c = (kHex8[i][2] + kHex8[j][2]) * 0.5;
                // Exactly one of a, b, c is zero: the direction the edge
                // runs in. That one gets the quadratic bubble and the
                // other two the usual linear factors.
                if (a == 0.0) {
                    set(8 + e, 0.25 * (1.0 - r * r) * (1.0 + b * s) * (1.0 + c * t),
                        -0.5 * r * (1.0 + b * s) * (1.0 + c * t),
                        0.25 * b * (1.0 - r * r) * (1.0 + c * t),
                        0.25 * c * (1.0 - r * r) * (1.0 + b * s));
                } else if (b == 0.0) {
                    set(8 + e, 0.25 * (1.0 + a * r) * (1.0 - s * s) * (1.0 + c * t),
                        0.25 * a * (1.0 - s * s) * (1.0 + c * t),
                        -0.5 * s * (1.0 + a * r) * (1.0 + c * t),
                        0.25 * c * (1.0 + a * r) * (1.0 - s * s));
                } else {
                    set(8 + e, 0.25 * (1.0 + a * r) * (1.0 + b * s) * (1.0 - t * t),
                        0.25 * a * (1.0 + b * s) * (1.0 - t * t),
                        0.25 * b * (1.0 + a * r) * (1.0 - t * t),
                        -0.5 * t * (1.0 + a * r) * (1.0 + b * s));
                }
            }
            return;
        }
        case ElementShape::Wedge6: {
            const double area[3] = {1.0 - r - s, r, s};
            const double da[3][2] = {{-1.0, -1.0}, {1.0, 0.0}, {0.0, 1.0}};
            for (int side = 0; side < 2; ++side) {
                const double c = side == 0 ? -1.0 : 1.0;
                const double h = 0.5 * (1.0 + c * t);
                for (int i = 0; i < 3; ++i) {
                    set(side * 3 + i, area[i] * h, da[i][0] * h, da[i][1] * h, 0.5 * c * area[i]);
                }
            }
            return;
        }
        case ElementShape::Wedge15: {
            const double area[3] = {1.0 - r - s, r, s};
            const double da[3][2] = {{-1.0, -1.0}, {1.0, 0.0}, {0.0, 1.0}};
            const double bubble = 1.0 - t * t;
            for (int side = 0; side < 2; ++side) {
                const double c = side == 0 ? -1.0 : 1.0;
                const double h = 1.0 + c * t;
                for (int i = 0; i < 3; ++i) {
                    // Half the corner function of a quadratic triangle
                    // times the linear factor, less half the bubble --
                    // which is what makes the mid-height node's own
                    // function vanish at the corners.
                    const double value = 0.5 * area[i] * ((2.0 * area[i] - 1.0) * h - bubble);
                    const double dr =
                        0.5 * da[i][0] * ((4.0 * area[i] - 1.0) * h - bubble);
                    const double ds =
                        0.5 * da[i][1] * ((4.0 * area[i] - 1.0) * h - bubble);
                    const double dt = 0.5 * area[i] * ((2.0 * area[i] - 1.0) * c + 2.0 * t);
                    set(side * 3 + i, value, dr, ds, dt);
                }
            }
            for (int e = 0; e < 6; ++e) {
                const int i = kWedgeEdges[e][0] % 3;
                const int j = kWedgeEdges[e][1] % 3;
                const double c = e < 3 ? -1.0 : 1.0;
                const double h = 1.0 + c * t;
                set(6 + e, 2.0 * area[i] * area[j] * h,
                    2.0 * (da[i][0] * area[j] + area[i] * da[j][0]) * h,
                    2.0 * (da[i][1] * area[j] + area[i] * da[j][1]) * h,
                    2.0 * area[i] * area[j] * c);
            }
            for (int i = 0; i < 3; ++i) {
                set(12 + i, area[i] * bubble, da[i][0] * bubble, da[i][1] * bubble,
                    -2.0 * t * area[i]);
            }
            return;
        }
        case ElementShape::Pyr5: {
            // THE ONE ELEMENT WHOSE SHAPE FUNCTIONS ARE NOT POLYNOMIALS.
            // A pyramid is a hexahedron with one face collapsed to a
            // point, and the collapse leaves a 1/(1-t) in the base
            // functions. It is finite everywhere inside and singular
            // exactly at the apex, which no quadrature rule here samples;
            // the floor below keeps a caller who evaluates there from
            // getting an infinity instead of a diagnosis.
            const double gap = std::max(1.0 - t, 1e-12);
            const double ratio = r * s * t / gap;
            const double dratio_dr = s * t / gap;
            const double dratio_ds = r * t / gap;
            const double dratio_dt = r * s / (gap * gap);
            for (int i = 0; i < 4; ++i) {
                const double a = kQuad4[i][0];
                const double b = kQuad4[i][1];
                const double sign = a * b;
                set(i, 0.25 * ((1.0 + a * r) * (1.0 + b * s) - t + sign * ratio),
                    0.25 * (a * (1.0 + b * s) + sign * dratio_dr),
                    0.25 * (b * (1.0 + a * r) + sign * dratio_ds),
                    0.25 * (-1.0 + sign * dratio_dt));
            }
            set(4, t, 0.0, 0.0, 1.0);
            return;
        }
    }
}

bool Quadrature(ElementShape shape, int degree, std::vector<QuadraturePoint> *out,
                std::string *error) {
    out->clear();
    error->clear();
    // The default is the lowest degree that integrates that element's own
    // stiffness exactly when its geometry is not distorted: the strain
    // is one degree below the shape functions, the energy is twice that.
    if (degree <= 0) {
        switch (shape) {
            case ElementShape::Tri3:
            case ElementShape::Tet4:
                degree = 1;
                break;
            case ElementShape::Quad4:
            case ElementShape::Hex8:
            case ElementShape::Wedge6:
            case ElementShape::Pyr5:
                degree = 2;
                break;
            default:
                degree = 4;
                break;
        }
    }
    std::vector<double> line_at;
    std::vector<double> line_weight;
    std::vector<QuadraturePoint> face;
    switch (shape) {
        case ElementShape::Tri3:
        case ElementShape::Tri6:
            if (TriangleRule(degree, out)) return true;
            *error = std::string("no triangle rule of degree ") + std::to_string(degree);
            return false;
        case ElementShape::Tet4:
        case ElementShape::Tet10:
            if (TetrahedronRule(degree, out)) return true;
            *error = std::string("no tetrahedron rule of degree ") + std::to_string(degree);
            return false;
        case ElementShape::Quad4:
        case ElementShape::Quad8:
            if (!GaussLegendre(PointsForDegree(degree), &line_at, &line_weight)) {
                *error = std::string("no Gauss rule of degree ") + std::to_string(degree);
                return false;
            }
            for (std::size_t i = 0; i < line_at.size(); ++i) {
                for (std::size_t j = 0; j < line_at.size(); ++j) {
                    out->push_back({Vec3d{line_at[i], line_at[j], 0.0},
                                    line_weight[i] * line_weight[j]});
                }
            }
            return true;
        case ElementShape::Hex8:
        case ElementShape::Hex20:
        case ElementShape::Pyr5:
            // The pyramid needs two more degrees than it was asked for.
            // Its own Jacobian carries a factor of (1-zeta) squared --
            // the base shrinks to nothing at the apex -- so a rule that
            // integrates the integrand exactly still has that to account
            // for, and a one-point rule does not even get the reference
            // volume right.
            if (!GaussLegendre(PointsForDegree(shape == ElementShape::Pyr5 ? degree + 2 : degree),
                               &line_at, &line_weight)) {
                *error = std::string("no Gauss rule of degree ") + std::to_string(degree);
                return false;
            }
            for (std::size_t i = 0; i < line_at.size(); ++i) {
                for (std::size_t j = 0; j < line_at.size(); ++j) {
                    for (std::size_t k = 0; k < line_at.size(); ++k) {
                        // The pyramid's reference coordinate runs from 0
                        // at the base to 1 at the apex, so the third
                        // direction is mapped from [-1,1] onto [0,1) and
                        // the base is shrunk with it -- a conical product,
                        // which is what keeps the points inside the
                        // pyramid rather than inside the hexahedron it
                        // was collapsed from.
                        if (shape == ElementShape::Pyr5) {
                            const double height = 0.5 * (1.0 + line_at[k]);
                            const double scale = 1.0 - height;
                            out->push_back({Vec3d{line_at[i] * scale, line_at[j] * scale, height},
                                            line_weight[i] * line_weight[j] * line_weight[k] *
                                                0.5 * scale * scale});
                            continue;
                        }
                        out->push_back({Vec3d{line_at[i], line_at[j], line_at[k]},
                                        line_weight[i] * line_weight[j] * line_weight[k]});
                    }
                }
            }
            return true;
        case ElementShape::Wedge6:
        case ElementShape::Wedge15:
            if (!TriangleRule(degree, &face)) {
                *error = std::string("no triangle rule of degree ") + std::to_string(degree);
                return false;
            }
            if (!GaussLegendre(PointsForDegree(degree), &line_at, &line_weight)) {
                *error = std::string("no Gauss rule of degree ") + std::to_string(degree);
                return false;
            }
            for (const QuadraturePoint &p : face) {
                for (std::size_t k = 0; k < line_at.size(); ++k) {
                    out->push_back({Vec3d{p.at.x, p.at.y, line_at[k]}, p.weight * line_weight[k]});
                }
            }
            return true;
    }
    *error = "unknown element shape";
    return false;
}

double ElementJacobian(ElementShape shape, const std::vector<Vec3d> &nodes,
                       const std::vector<double> &dn_ref, std::vector<double> *out_dn_xyz) {
    const int count = ElementNodeCount(shape);
    const int dimension = ElementDimension(shape);
    out_dn_xyz->assign(Idx(count * 3), 0.0);
    double jacobian[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (int a = 0; a < count; ++a) {
        const Vec3d &p = nodes[Idx(a)];
        const double position[3] = {p.x, p.y, p.z};
        for (int i = 0; i < dimension; ++i) {
            for (int j = 0; j < dimension; ++j) {
                jacobian[i][j] += dn_ref[Idx(a * 3 + i)] * position[j];
            }
        }
    }
    double inverse[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    double determinant = 0.0;
    if (dimension == 2) {
        determinant = jacobian[0][0] * jacobian[1][1] - jacobian[0][1] * jacobian[1][0];
        if (determinant == 0.0) return 0.0;
        inverse[0][0] = jacobian[1][1] / determinant;
        inverse[0][1] = -jacobian[0][1] / determinant;
        inverse[1][0] = -jacobian[1][0] / determinant;
        inverse[1][1] = jacobian[0][0] / determinant;
    } else {
        determinant = jacobian[0][0] * (jacobian[1][1] * jacobian[2][2] - jacobian[1][2] * jacobian[2][1]) -
                      jacobian[0][1] * (jacobian[1][0] * jacobian[2][2] - jacobian[1][2] * jacobian[2][0]) +
                      jacobian[0][2] * (jacobian[1][0] * jacobian[2][1] - jacobian[1][1] * jacobian[2][0]);
        if (determinant == 0.0) return 0.0;
        inverse[0][0] = (jacobian[1][1] * jacobian[2][2] - jacobian[1][2] * jacobian[2][1]) / determinant;
        inverse[0][1] = (jacobian[0][2] * jacobian[2][1] - jacobian[0][1] * jacobian[2][2]) / determinant;
        inverse[0][2] = (jacobian[0][1] * jacobian[1][2] - jacobian[0][2] * jacobian[1][1]) / determinant;
        inverse[1][0] = (jacobian[1][2] * jacobian[2][0] - jacobian[1][0] * jacobian[2][2]) / determinant;
        inverse[1][1] = (jacobian[0][0] * jacobian[2][2] - jacobian[0][2] * jacobian[2][0]) / determinant;
        inverse[1][2] = (jacobian[0][2] * jacobian[1][0] - jacobian[0][0] * jacobian[1][2]) / determinant;
        inverse[2][0] = (jacobian[1][0] * jacobian[2][1] - jacobian[1][1] * jacobian[2][0]) / determinant;
        inverse[2][1] = (jacobian[0][1] * jacobian[2][0] - jacobian[0][0] * jacobian[2][1]) / determinant;
        inverse[2][2] = (jacobian[0][0] * jacobian[1][1] - jacobian[0][1] * jacobian[1][0]) / determinant;
    }
    // THE INVERSE IS USED TRANSPOSED, AND THIS IS THE PLACE TO SAY WHY.
    // `jacobian[i][j]` holds dx_j/dxi_i -- reference index first, real
    // index second -- so its inverse holds dxi_i/dx_j at [j][i], and the
    // chain rule wants dN/dx_j = sum_i dN/dxi_i * dxi_i/dx_j.
    //
    // Indexing it the other way round is a bug that hides: the shape
    // derivatives still sum to zero, so an element still carries no force
    // under a rigid *translation* and its row sums still vanish. What
    // breaks is rigid rotation, which stops being strain-free, and the
    // element loses three of its six zero eigenvalues while remaining
    // symmetric and positive. It was written the wrong way here first and
    // found by the rank check rather than by reading.
    for (int a = 0; a < count; ++a) {
        for (int j = 0; j < dimension; ++j) {
            double sum = 0.0;
            for (int i = 0; i < dimension; ++i) {
                sum += dn_ref[Idx(a * 3 + i)] * inverse[j][i];
            }
            (*out_dn_xyz)[Idx(a * 3 + j)] = sum;
        }
    }
    return determinant;
}

void StrainDisplacement(ElementShape shape, const std::vector<double> &dn_xyz,
                        std::vector<double> *out) {
    const int count = ElementNodeCount(shape);
    const int dimension = ElementDimension(shape);
    const int rows = dimension == 2 ? 3 : 6;
    const int columns = count * dimension;
    out->assign(Idx(rows * columns), 0.0);
    double *b = out->data();
    for (int a = 0; a < count; ++a) {
        const double dx = dn_xyz[Idx(a * 3 + 0)];
        const double dy = dn_xyz[Idx(a * 3 + 1)];
        const double dz = dn_xyz[Idx(a * 3 + 2)];
        if (dimension == 2) {
            b[Idx(0 * columns + a * 2 + 0)] = dx;
            b[Idx(1 * columns + a * 2 + 1)] = dy;
            b[Idx(2 * columns + a * 2 + 0)] = dy;
            b[Idx(2 * columns + a * 2 + 1)] = dx;
            continue;
        }
        b[Idx(0 * columns + a * 3 + 0)] = dx;
        b[Idx(1 * columns + a * 3 + 1)] = dy;
        b[Idx(2 * columns + a * 3 + 2)] = dz;
        b[Idx(3 * columns + a * 3 + 0)] = dy;
        b[Idx(3 * columns + a * 3 + 1)] = dx;
        b[Idx(4 * columns + a * 3 + 1)] = dz;
        b[Idx(4 * columns + a * 3 + 2)] = dy;
        b[Idx(5 * columns + a * 3 + 0)] = dz;
        b[Idx(5 * columns + a * 3 + 2)] = dx;
    }
}

void ConstitutiveMatrix2D(const Material &material, PlaneKind plane, double out[3][3]) {
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) out[i][j] = 0.0;
    }
    const double e = material.youngs_modulus;
    const double v = material.poissons_ratio;
    if (plane == PlaneKind::Stress) {
        const double factor = e / (1.0 - v * v);
        out[0][0] = factor;
        out[0][1] = factor * v;
        out[1][0] = factor * v;
        out[1][1] = factor;
        out[2][2] = factor * (1.0 - v) * 0.5;
        return;
    }
    const double factor = e / ((1.0 + v) * (1.0 - 2.0 * v));
    out[0][0] = factor * (1.0 - v);
    out[0][1] = factor * v;
    out[1][0] = factor * v;
    out[1][1] = factor * (1.0 - v);
    out[2][2] = factor * (1.0 - 2.0 * v) * 0.5;
}

bool ElementVolume(ElementShape shape, const std::vector<Vec3d> &nodes,
                   const ElementOptions &options, double *out, std::string *error) {
    *out = 0.0;
    const int count = ElementNodeCount(shape);
    if (static_cast<int>(nodes.size()) != count) {
        *error = std::string(ElementShapeName(shape)) + " wants " + std::to_string(count) +
                 " nodes and was given " + std::to_string(nodes.size());
        return false;
    }
    std::vector<QuadraturePoint> rule;
    if (!Quadrature(shape, options.integration_degree, &rule, error)) return false;
    std::vector<double> n;
    std::vector<double> dn;
    std::vector<double> dn_xyz;
    for (const QuadraturePoint &point : rule) {
        ShapeFunctions(shape, point.at, &n, &dn);
        const double determinant = ElementJacobian(shape, nodes, dn, &dn_xyz);
        *out += determinant * point.weight;
    }
    if (ElementDimension(shape) == 2) *out *= options.thickness;
    return true;
}

namespace {

bool StiffnessWith(ElementShape shape, const std::vector<Vec3d> &nodes, const double d3[6][6],
                   const double d2[3][3], const ElementOptions &options, std::vector<double> *out,
                   std::string *error) {
    const int count = ElementNodeCount(shape);
    const int dimension = ElementDimension(shape);
    const int size = count * dimension;
    out->assign(Idx(size * size), 0.0);
    if (static_cast<int>(nodes.size()) != count) {
        *error = std::string(ElementShapeName(shape)) + " wants " + std::to_string(count) +
                 " nodes and was given " + std::to_string(nodes.size());
        return false;
    }

    std::vector<QuadraturePoint> rule;
    if (!Quadrature(shape, options.integration_degree, &rule, error)) return false;

    const int rows = dimension == 2 ? 3 : 6;

    // The shape derivatives at every point, kept because B-bar needs a
    // second pass over them and recomputing is both slower and a second
    // chance to compute them differently.
    std::vector<std::vector<double>> derivative(rule.size());
    std::vector<double> determinant(rule.size(), 0.0);
    std::vector<double> n;
    std::vector<double> dn;
    double volume = 0.0;
    for (std::size_t q = 0; q < rule.size(); ++q) {
        ShapeFunctions(shape, rule[q].at, &n, &dn);
        determinant[q] = ElementJacobian(shape, nodes, dn, &derivative[q]);
        if (!(determinant[q] > 0.0)) {
            *error = std::string(ElementShapeName(shape)) +
                     " is turned inside out or folded: its Jacobian is " +
                     std::to_string(determinant[q]) + " at a quadrature point";
            return false;
        }
        volume += determinant[q] * rule[q].weight;
    }

    // B-bar: the volume-averaged derivatives, which replace the
    // volumetric part of the strain at every point.
    std::vector<double> averaged(Idx(count * 3), 0.0);
    if (options.b_bar) {
        for (std::size_t q = 0; q < rule.size(); ++q) {
            const double scale = determinant[q] * rule[q].weight;
            for (int i = 0; i < count * 3; ++i) {
                averaged[Idx(i)] += derivative[q][Idx(i)] * scale;
            }
        }
        for (double &value : averaged) value /= volume;
    }

    std::vector<double> b;
    std::vector<double> db;
    std::vector<double> adjusted;
    for (std::size_t q = 0; q < rule.size(); ++q) {
        const std::vector<double> *use = &derivative[q];
        if (options.b_bar) {
            // Only the dilatational part is averaged. Writing it as a
            // correction to the derivatives themselves works because the
            // dilatational part of B is built from exactly those.
            adjusted = derivative[q];
            use = &adjusted;
        }
        StrainDisplacement(shape, *use, &b);
        if (options.b_bar) {
            const int columns = count * dimension;
            const double share = 1.0 / static_cast<double>(dimension);
            for (int a = 0; a < count; ++a) {
                for (int j = 0; j < dimension; ++j) {
                    const double correction =
                        (averaged[Idx(a * 3 + j)] - derivative[q][Idx(a * 3 + j)]) * share;
                    for (int i = 0; i < dimension; ++i) {
                        b[Idx(i * columns + a * dimension + j)] += correction;
                    }
                }
            }
        }
        const int columns = count * dimension;
        const double scale = determinant[q] * rule[q].weight *
                             (dimension == 2 ? options.thickness : 1.0);
        db.assign(Idx(rows * columns), 0.0);
        for (int i = 0; i < rows; ++i) {
            for (int j = 0; j < columns; ++j) {
                double sum = 0.0;
                for (int k = 0; k < rows; ++k) {
                    const double value = dimension == 2 ? d2[i][k] : d3[i][k];
                    sum += value * b[Idx(k * columns + j)];
                }
                db[Idx(i * columns + j)] = sum;
            }
        }
        for (int i = 0; i < columns; ++i) {
            for (int j = i; j < columns; ++j) {
                double sum = 0.0;
                for (int k = 0; k < rows; ++k) {
                    sum += b[Idx(k * columns + i)] * db[Idx(k * columns + j)];
                }
                (*out)[Idx(i * size + j)] += sum * scale;
            }
        }
    }
    // Symmetric by construction, and filled in as such rather than
    // computed twice: the two halves would differ in the last bit and an
    // assembled matrix that is almost symmetric is not one.
    for (int i = 0; i < size; ++i) {
        for (int j = 0; j < i; ++j) {
            (*out)[Idx(i * size + j)] = (*out)[Idx(j * size + i)];
        }
    }
    return true;
}

}  // namespace

bool ElementStiffness(ElementShape shape, const std::vector<Vec3d> &nodes,
                      const Material &material, const ElementOptions &options,
                      std::vector<double> *out, std::string *error) {
    error->clear();
    if (!material.IsValid(error)) return false;
    double d3[6][6] = {};
    double d2[3][3] = {};
    if (ElementDimension(shape) == 3) {
        ConstitutiveMatrix(material, d3);
    } else {
        ConstitutiveMatrix2D(material, options.plane, d2);
    }
    return StiffnessWith(shape, nodes, d3, d2, options, out, error);
}

bool ElementStiffness(ElementShape shape, const std::vector<Vec3d> &nodes,
                      const double constitutive[6][6], const ElementOptions &options,
                      std::vector<double> *out, std::string *error) {
    error->clear();
    if (ElementDimension(shape) != 3) {
        *error = std::string(ElementShapeName(shape)) +
                 " is two-dimensional and wants a 3x3 constitutive matrix, not a 6x6 one";
        return false;
    }
    const double d2[3][3] = {};
    return StiffnessWith(shape, nodes, constitutive, d2, options, out, error);
}

}  // namespace fem
