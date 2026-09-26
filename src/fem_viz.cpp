#include "fem_viz.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>

namespace fem {
namespace {

inline std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// The six faces of a Hex8, each as four local node indices in the order
// that makes the face wind counter-clockwise seen from *outside* the
// element. That orientation is what makes the extracted surface's normals
// point outward without a separate fix-up pass.
constexpr int kHexFace[6][4] = {
    {0, 3, 2, 1},  // -zeta
    {4, 5, 6, 7},  // +zeta
    {0, 1, 5, 4},  // -eta
    {3, 7, 6, 2},  // +eta
    {0, 4, 7, 3},  // -xi
    {1, 2, 6, 5},  // +xi
};

// Viridis, sampled at 9 stops and interpolated between them. The full
// 256-entry table would be more faithful, but the eye cannot resolve the
// difference in a shaded 3D view and this keeps the constant readable.
constexpr double kViridis[9][3] = {
    {0.267, 0.005, 0.329}, {0.283, 0.141, 0.458}, {0.254, 0.265, 0.530}, {0.207, 0.372, 0.553},
    {0.164, 0.471, 0.558}, {0.128, 0.567, 0.551}, {0.135, 0.659, 0.518}, {0.267, 0.749, 0.441},
    {0.478, 0.821, 0.318},
};

unsigned char ToByte(double v) {
    return static_cast<unsigned char>(cad::Clamp(v, 0.0, 1.0) * 255.0 + 0.5);
}

}  // namespace

void MapColor(ColorMap map, double t, unsigned char out_rgba[4]) {
    t = cad::Clamp(t, 0.0, 1.0);
    out_rgba[3] = 255;
    switch (map) {
        case ColorMap::Viridis: {
            const double scaled = t * 8.0;
            const int lower = std::min(7, static_cast<int>(scaled));
            const double frac = scaled - static_cast<double>(lower);
            for (int c = 0; c < 3; ++c) {
                const double a = kViridis[lower][c];
                const double b = kViridis[lower + 1][c];
                out_rgba[c] = ToByte(a + (b - a) * frac);
            }
            // The last stop is the darkest end of viridis' yellow; push
            // the very top toward it so the maximum reads as a peak.
            if (t > 0.95) {
                const double blend = (t - 0.95) / 0.05;
                out_rgba[0] = ToByte((static_cast<double>(out_rgba[0]) / 255.0) * (1.0 - blend) + 0.993 * blend);
                out_rgba[1] = ToByte((static_cast<double>(out_rgba[1]) / 255.0) * (1.0 - blend) + 0.906 * blend);
                out_rgba[2] = ToByte((static_cast<double>(out_rgba[2]) / 255.0) * (1.0 - blend) + 0.144 * blend);
            }
            return;
        }
        case ColorMap::BlueToRed: {
            // Blue -> cyan -> green -> yellow -> red, the familiar one.
            const double x = t * 4.0;
            double r = 0.0, g = 0.0, b = 0.0;
            if (x < 1.0) {
                r = 0.0; g = x; b = 1.0;
            } else if (x < 2.0) {
                r = 0.0; g = 1.0; b = 2.0 - x;
            } else if (x < 3.0) {
                r = x - 2.0; g = 1.0; b = 0.0;
            } else {
                r = 1.0; g = 4.0 - x; b = 0.0;
            }
            out_rgba[0] = ToByte(r);
            out_rgba[1] = ToByte(g);
            out_rgba[2] = ToByte(b);
            return;
        }
        case ColorMap::Greyscale: {
            const unsigned char v = ToByte(0.1 + 0.85 * t);
            out_rgba[0] = out_rgba[1] = out_rgba[2] = v;
            return;
        }
    }
}

ResultMesh BuildResultMesh(const Model &model, const Result &result, const ResultMeshOptions &options) {
    ResultMesh mesh;
    if (!result.ok || result.displacements.size() != model.nodes.size()) return mesh;

    // --- Surface extraction -------------------------------------------
    // A face is on the surface exactly when it belongs to one element.
    // Keyed by its sorted node indices, so the two elements sharing an
    // interior face agree on the key regardless of the order each lists
    // it in.
    std::map<std::array<int, 4>, std::pair<std::array<int, 4>, int>> face_count;
    for (const Element &element : model.elements) {
        for (int f = 0; f < 6; ++f) {
            std::array<int, 4> face{};
            for (int c = 0; c < 4; ++c) face[Idx(c)] = element.nodes[Idx(kHexFace[f][c])];
            std::array<int, 4> key = face;
            std::sort(key.begin(), key.end());
            auto found = face_count.find(key);
            if (found == face_count.end()) {
                face_count.emplace(key, std::make_pair(face, 1));
            } else {
                ++found->second.second;
            }
        }
    }

    std::vector<std::array<int, 4>> surface_faces;
    for (const auto &entry : face_count) {
        if (entry.second.second == 1) surface_faces.push_back(entry.second.first);
    }
    if (surface_faces.empty()) return mesh;

    // Only the nodes actually on the surface become vertices, remapped to
    // a compact range. An interior node of a solid mesh is never drawn,
    // and for a mesh of any size the interior is most of it.
    std::vector<int> remap(model.nodes.size(), -1);
    std::vector<int> surface_nodes;
    for (const std::array<int, 4> &face : surface_faces) {
        for (int c = 0; c < 4; ++c) {
            const int node = face[Idx(c)];
            if (remap[Idx(node)] < 0) {
                remap[Idx(node)] = static_cast<int>(surface_nodes.size());
                surface_nodes.push_back(node);
            }
        }
    }

    // --- The scalar field ---------------------------------------------
    auto field_at = [&](int node) {
        switch (options.field) {
            case ResultField::VonMises:
                return result.nodal_stress[Idx(node)].VonMises();
            case ResultField::DisplacementMagnitude:
                return result.displacements[Idx(node)].Length();
            case ResultField::MaxPrincipalStress: {
                double principal[3];
                result.nodal_stress[Idx(node)].Principal(principal);
                return principal[0];
            }
        }
        return 0.0;
    };

    double field_min = options.range_min;
    double field_max = options.range_max;
    if (options.auto_range) {
        field_min = field_at(surface_nodes[0]);
        field_max = field_min;
        for (int node : surface_nodes) {
            const double v = field_at(node);
            field_min = std::min(field_min, v);
            field_max = std::max(field_max, v);
        }
    }
    // A constant field would divide by zero; widen it so everything maps
    // to the middle of the ramp rather than to NaN.
    if (!(field_max > field_min)) {
        const double pad = std::max(1e-12, std::fabs(field_max) * 1e-6);
        field_min -= pad;
        field_max += pad;
    }
    mesh.field_min = field_min;
    mesh.field_max = field_max;

    // --- Displacement scaling -----------------------------------------
    double scale = options.displacement_scale;
    if (options.undeformed) {
        scale = 0.0;
    } else if (options.auto_scale_displacement) {
        const double diagonal = model.BoundingBox().Diagonal();
        const double largest = result.max_displacement_magnitude;
        scale = (largest > 0.0 && diagonal > 0.0) ? (options.auto_scale_fraction * diagonal / largest) : 1.0;
    }
    mesh.displacement_scale_used = scale;

    // --- Vertices -------------------------------------------------------
    const std::size_t vertex_count = surface_nodes.size();
    mesh.positions.assign(vertex_count * 3, 0.0f);
    mesh.normals.assign(vertex_count * 3, 0.0f);
    mesh.colors.assign(vertex_count * 4, 255);
    for (std::size_t v = 0; v < vertex_count; ++v) {
        const int node = surface_nodes[v];
        const cad::Vec3d p = model.nodes[Idx(node)] + result.displacements[Idx(node)] * scale;
        mesh.positions[v * 3 + 0] = static_cast<float>(p.x);
        mesh.positions[v * 3 + 1] = static_cast<float>(p.y);
        mesh.positions[v * 3 + 2] = static_cast<float>(p.z);
        const double t = (field_at(node) - field_min) / (field_max - field_min);
        unsigned char rgba[4];
        MapColor(options.color_map, t, rgba);
        for (int c = 0; c < 4; ++c) mesh.colors[v * 4 + static_cast<std::size_t>(c)] = rgba[c];
    }

    // --- Triangles ------------------------------------------------------
    mesh.indices.reserve(surface_faces.size() * 6);
    for (const std::array<int, 4> &face : surface_faces) {
        const unsigned int a = static_cast<unsigned int>(remap[Idx(face[0])]);
        const unsigned int b = static_cast<unsigned int>(remap[Idx(face[1])]);
        const unsigned int c = static_cast<unsigned int>(remap[Idx(face[2])]);
        const unsigned int d = static_cast<unsigned int>(remap[Idx(face[3])]);
        mesh.indices.push_back(a);
        mesh.indices.push_back(b);
        mesh.indices.push_back(c);
        mesh.indices.push_back(a);
        mesh.indices.push_back(c);
        mesh.indices.push_back(d);
    }

    // Smooth normals: the area-weighted sum of each incident triangle's
    // own normal. Computed from the *deformed* positions, so the shading
    // follows the shape actually being drawn.
    for (std::size_t t = 0; t + 2 < mesh.indices.size(); t += 3) {
        const std::size_t i0 = mesh.indices[t];
        const std::size_t i1 = mesh.indices[t + 1];
        const std::size_t i2 = mesh.indices[t + 2];
        // Explicit widening: the mesh stores float (that is what the GPU
        // takes) while the geometry below is computed in double, and
        // -Wdouble-promotion rightly refuses to do that silently.
        auto position_at = [&mesh](std::size_t i) {
            return cad::Vec3d{static_cast<double>(mesh.positions[i * 3]),
                              static_cast<double>(mesh.positions[i * 3 + 1]),
                              static_cast<double>(mesh.positions[i * 3 + 2])};
        };
        const cad::Vec3d normal = (position_at(i1) - position_at(i0)).Cross(position_at(i2) - position_at(i0));
        for (std::size_t i : {i0, i1, i2}) {
            mesh.normals[i * 3 + 0] += static_cast<float>(normal.x);
            mesh.normals[i * 3 + 1] += static_cast<float>(normal.y);
            mesh.normals[i * 3 + 2] += static_cast<float>(normal.z);
        }
    }
    for (std::size_t v = 0; v < vertex_count; ++v) {
        const cad::Vec3d n{static_cast<double>(mesh.normals[v * 3]),
                           static_cast<double>(mesh.normals[v * 3 + 1]),
                           static_cast<double>(mesh.normals[v * 3 + 2])};
        const cad::Vec3d unit = n.Normalized();
        mesh.normals[v * 3 + 0] = static_cast<float>(unit.x);
        mesh.normals[v * 3 + 1] = static_cast<float>(unit.y);
        mesh.normals[v * 3 + 2] = static_cast<float>(unit.z);
    }

    mesh.surface_vertex_count = static_cast<int>(vertex_count);
    mesh.surface_triangle_count = static_cast<int>(mesh.indices.size() / 3);
    return mesh;
}

}  // namespace fem
