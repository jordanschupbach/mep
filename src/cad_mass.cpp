#include "cad_mass.h"

#include <algorithm>
#include <cmath>

namespace cad {
namespace {

// Everything below is one of these: an integral over a face's parameter
// rectangle of some scalar built from the point and the vector area
// element. Collecting them into one accumulator means the quadrature loop
// is written once, and every moment is evaluated from the same samples.
struct Accumulator {
    double volume = 0.0;
    double area = 0.0;
    Vec3d first;          // integral of (x, y, z) dV
    Vec3d second_square;  // integral of (x^2, y^2, z^2) dV
    Vec3d second_cross;   // integral of (xy, yz, zx) dV
};

void AccumulateFace(const MassFace &face, const MassOptions &options, Accumulator *out) {
    double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
    face.surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    const int cells = std::max(1, options.subdivisions);
    const int order = std::max(1, options.quadrature_order);
    std::vector<double> nodes;
    std::vector<double> weights;
    GaussLegendreRule(order, &nodes, &weights);

    const double du = (u_hi - u_lo) / static_cast<double>(cells);
    const double dv = (v_hi - v_lo) / static_cast<double>(cells);
    const double sign = face.reversed ? -1.0 : 1.0;

    for (int cu = 0; cu < cells; ++cu) {
        const double u0 = u_lo + du * static_cast<double>(cu);
        for (int cv = 0; cv < cells; ++cv) {
            const double v0 = v_lo + dv * static_cast<double>(cv);
            for (std::size_t i = 0; i < nodes.size(); ++i) {
                // Map the reference node onto this cell.
                const double u = u0 + du * 0.5 * (nodes[i] + 1.0);
                const double wu = weights[i] * du * 0.5;
                for (std::size_t j = 0; j < nodes.size(); ++j) {
                    const double v = v0 + dv * 0.5 * (nodes[j] + 1.0);
                    const double weight = wu * weights[j] * dv * 0.5;

                    std::vector<std::vector<Vec3d>> ders;
                    face.surface->Derivatives(u, v, 1, &ders);
                    const Vec3d p = ders[0][0];
                    // n dA = (Su x Sv) du dv exactly -- no normalization,
                    // because the magnitude carries the area element.
                    const Vec3d area_element = ders[1][0].Cross(ders[0][1]) * sign;

                    out->area += area_element.Length() * weight;
                    // V = (1/3) * integral of r . n dA.
                    out->volume += (p.Dot(area_element) / 3.0) * weight;
                    // First moments: integral of x dV = integral of
                    // (x^2/2) n_x dA, and likewise for y and z.
                    out->first.x += 0.5 * p.x * p.x * area_element.x * weight;
                    out->first.y += 0.5 * p.y * p.y * area_element.y * weight;
                    out->first.z += 0.5 * p.z * p.z * area_element.z * weight;
                    // Second moments: integral of x^2 dV = integral of
                    // (x^3/3) n_x dA.
                    out->second_square.x += (p.x * p.x * p.x / 3.0) * area_element.x * weight;
                    out->second_square.y += (p.y * p.y * p.y / 3.0) * area_element.y * weight;
                    out->second_square.z += (p.z * p.z * p.z / 3.0) * area_element.z * weight;
                    // Products: integral of xy dV = integral of
                    // (x^2 y / 2) n_x dA.
                    out->second_cross.x += 0.5 * p.x * p.x * p.y * area_element.x * weight;
                    out->second_cross.y += 0.5 * p.y * p.y * p.z * area_element.y * weight;
                    out->second_cross.z += 0.5 * p.z * p.z * p.x * area_element.z * weight;
                }
            }
        }
    }
}

}  // namespace

void MassProperties::PrincipalMoments(double *out_sorted_three) const {
    // Eigenvalues of a symmetric 3x3, by the same closed-form
    // trigonometric solution fem_solve.cpp uses for stress. An inertia
    // tensor is always symmetric and positive semi-definite, so the
    // cubic's roots are real and the formula is well conditioned.
    const Mat3d &m = inertia_centroid;
    const double p1 = m.m[0][1] * m.m[0][1] + m.m[0][2] * m.m[0][2] + m.m[1][2] * m.m[1][2];
    if (p1 <= 0.0) {
        out_sorted_three[0] = m.m[0][0];
        out_sorted_three[1] = m.m[1][1];
        out_sorted_three[2] = m.m[2][2];
        std::sort(out_sorted_three, out_sorted_three + 3);
        return;
    }
    const double q = (m.m[0][0] + m.m[1][1] + m.m[2][2]) / 3.0;
    const double p2 = (m.m[0][0] - q) * (m.m[0][0] - q) + (m.m[1][1] - q) * (m.m[1][1] - q) +
                      (m.m[2][2] - q) * (m.m[2][2] - q) + 2.0 * p1;
    const double p = std::sqrt(p2 / 6.0);
    Mat3d b = m;
    for (int i = 0; i < 3; ++i) b.m[i][i] -= q;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) b.m[i][j] /= p;
    }
    const double r = Clamp(b.Determinant() * 0.5, -1.0, 1.0);
    const double phi = std::acos(r) / 3.0;
    out_sorted_three[2] = q + 2.0 * p * std::cos(phi);
    out_sorted_three[0] = q + 2.0 * p * std::cos(phi + 2.0 * kPi / 3.0);
    out_sorted_three[1] = 3.0 * q - out_sorted_three[0] - out_sorted_three[2];
    std::sort(out_sorted_three, out_sorted_three + 3);
}

bool ComputeMassProperties(const std::vector<MassFace> &faces, MassProperties *out, const MassOptions &options) {
    Accumulator accumulator;
    for (const MassFace &face : faces) {
        if (face.surface == nullptr) return false;
        AccumulateFace(face, options, &accumulator);
    }

    out->density = options.density;
    out->volume = accumulator.volume;
    out->area = accumulator.area;
    out->mass = accumulator.volume * options.density;
    if (std::fabs(accumulator.volume) > 1e-300) {
        out->centroid = accumulator.first / accumulator.volume;
    } else {
        out->centroid = Vec3d{};
    }

    // Inertia about the origin, from the second moments. The diagonal
    // terms pair up the two coordinates *other* than the axis; the
    // off-diagonal products carry a minus sign by the standard definition.
    const double ixx = accumulator.second_square.y + accumulator.second_square.z;
    const double iyy = accumulator.second_square.x + accumulator.second_square.z;
    const double izz = accumulator.second_square.x + accumulator.second_square.y;
    Mat3d origin = Mat3d::Zero();
    origin.m[0][0] = ixx * options.density;
    origin.m[1][1] = iyy * options.density;
    origin.m[2][2] = izz * options.density;
    origin.m[0][1] = origin.m[1][0] = -accumulator.second_cross.x * options.density;
    origin.m[1][2] = origin.m[2][1] = -accumulator.second_cross.y * options.density;
    origin.m[0][2] = origin.m[2][0] = -accumulator.second_cross.z * options.density;
    out->inertia_origin = origin;

    // Parallel axis theorem, shifting to the centroid.
    const double mass = out->mass;
    const Vec3d c = out->centroid;
    Mat3d centroidal = origin;
    centroidal.m[0][0] -= mass * (c.y * c.y + c.z * c.z);
    centroidal.m[1][1] -= mass * (c.x * c.x + c.z * c.z);
    centroidal.m[2][2] -= mass * (c.x * c.x + c.y * c.y);
    centroidal.m[0][1] += mass * c.x * c.y;
    centroidal.m[1][0] += mass * c.x * c.y;
    centroidal.m[1][2] += mass * c.y * c.z;
    centroidal.m[2][1] += mass * c.y * c.z;
    centroidal.m[0][2] += mass * c.x * c.z;
    centroidal.m[2][0] += mass * c.x * c.z;
    out->inertia_centroid = centroidal;
    return true;
}

double ComputeArea(const std::vector<MassFace> &faces, const MassOptions &options) {
    Accumulator accumulator;
    for (const MassFace &face : faces) {
        if (face.surface == nullptr) continue;
        AccumulateFace(face, options, &accumulator);
    }
    return accumulator.area;
}

}  // namespace cad
