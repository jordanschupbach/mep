#include "fem_render.h"

#include "fem_elem.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace fem {
namespace {

using cad::Vec3d;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// The arrays are floats because that is what a renderer wants; the
// geometry checks below are doubles because an area summed over ten
// thousand triangles in single precision is not worth checking. The cast
// is explicit throughout because -Wdouble-promotion is on, and it is on
// for the good reason that a silent widening usually means someone
// forgot which precision they were in.
double D(float v) { return static_cast<double>(v); }

// Corner-node splits. Each row indexes into the element's node list.
//
// THE SIX-TETRAHEDRON HEXAHEDRON SPLIT IS NOT ARBITRARY: all six share
// the 0-6 diagonal, which makes the split *compatible* -- neighbouring
// hexahedra cut their shared face along the same diagonal, so the tets
// meet face to face and the interior faces cancel when the boundary is
// extracted. A split chosen per element would leave the boundary
// extraction reporting interior faces as surface, which draws a solid
// full of internal walls.
const int kHexSplit[6][4] = {{0, 1, 2, 6}, {0, 2, 3, 6}, {0, 3, 7, 6},
                             {0, 7, 4, 6}, {0, 4, 5, 6}, {0, 5, 1, 6}};
const int kWedgeSplit[3][4] = {{0, 1, 2, 5}, {0, 1, 5, 4}, {0, 4, 5, 3}};
const int kPyramidSplit[2][4] = {{0, 1, 2, 4}, {0, 2, 3, 4}};
// A quadratic tetrahedron on its own mid-edge nodes: four corner tets and
// an octahedron in the middle cut into four. Node order is the element
// library's -- corners 0..3 then edges (0,1), (1,2), (0,2), (0,3), (1,3),
// (2,3) as nodes 4..9.
const int kTet10Split[8][4] = {{0, 4, 6, 7}, {4, 1, 5, 8}, {6, 5, 2, 9}, {7, 8, 9, 3},
                               {4, 6, 7, 8}, {4, 6, 8, 5}, {6, 7, 8, 9}, {6, 8, 5, 9}};

double Volume(const Vec3d &a, const Vec3d &b, const Vec3d &c, const Vec3d &d) {
    return (b - a).Cross(c - a).Dot(d - a) / 6.0;
}

struct Builder {
    RenderMesh *out = nullptr;
    // THE SAME POINTS IN FULL PRECISION. The arrays a renderer wants are
    // floats, and an area summed back out of them over ten thousand
    // triangles carries about seven digits -- which is plenty to draw
    // with and not enough to check against a closed form. The section
    // area of a unit cube came out 2.5e-09 wrong for no reason but that.
    // These are the numbers the self-checks are computed from.
    std::vector<Vec3d> exact;
    ColorMap map = ColorMap::Viridis;
    double low = 0.0;
    double high = 1.0;

    unsigned int Vertex(const Vec3d &position, double value) {
        const unsigned int index = static_cast<unsigned int>(out->positions.size() / 3);
        exact.push_back(position);
        out->positions.push_back(static_cast<float>(position.x));
        out->positions.push_back(static_cast<float>(position.y));
        out->positions.push_back(static_cast<float>(position.z));
        out->normals.insert(out->normals.end(), {0.0f, 0.0f, 0.0f});
        unsigned char rgba[4];
        const double span = high - low;
        // A CONSTANT FIELD IS NOT AN ERROR AND MUST NOT DIVIDE BY ZERO.
        // It happens on every uniform-stress model and on every
        // isosurface coloured by the field it is an isosurface of, and
        // the sensible picture is the middle of the map rather than
        // whichever end a NaN lands on.
        const double t = span > 0.0 ? (value - low) / span : 0.5;
        MapColor(map, t, rgba);
        out->colors.insert(out->colors.end(), rgba, rgba + 4);
        return index;
    }

    void Triangle(unsigned int a, unsigned int b, unsigned int c) {
        out->indices.push_back(a);
        out->indices.push_back(b);
        out->indices.push_back(c);
    }
};

void FinishNormals(RenderMesh *out, const std::vector<Vec3d> &exact) {
    const std::size_t vertices = out->positions.size() / 3;
    out->normals.assign(vertices * 3, 0.0f);
    out->area = 0.0;
    out->enclosed_volume = 0.0;
    auto point = [&](unsigned int i) { return exact[i]; };
    for (std::size_t t = 0; t + 2 < out->indices.size(); t += 3) {
        const unsigned int ia = out->indices[t], ib = out->indices[t + 1],
                           ic = out->indices[t + 2];
        const Vec3d a = point(ia), b = point(ib), c = point(ic);
        const Vec3d cross = (b - a).Cross(c - a);
        out->area += 0.5 * cross.Length();
        // The divergence theorem, as a sum over triangles: the enclosed
        // volume of a closed surface is the sum of a.(b x c)/6. On an
        // open surface it is meaningless, which is why the callers that
        // produce one say so rather than this trying to guess.
        out->enclosed_volume += a.Dot(b.Cross(c)) / 6.0;
        for (const unsigned int i : {ia, ib, ic}) {
            // Weighted by the cross product's length, which is twice the
            // triangle's area: a vertex where a sliver meets a large
            // triangle should take its normal mostly from the large one.
            out->normals[i * 3 + 0] += static_cast<float>(cross.x);
            out->normals[i * 3 + 1] += static_cast<float>(cross.y);
            out->normals[i * 3 + 2] += static_cast<float>(cross.z);
        }
    }
    for (std::size_t i = 0; i < vertices; ++i) {
        const double x = D(out->normals[i * 3]), y = D(out->normals[i * 3 + 1]),
                     z = D(out->normals[i * 3 + 2]);
        const double length = std::sqrt(x * x + y * y + z * z);
        if (!(length > 0.0)) continue;
        out->normals[i * 3 + 0] = static_cast<float>(x / length);
        out->normals[i * 3 + 1] = static_cast<float>(y / length);
        out->normals[i * 3 + 2] = static_cast<float>(z / length);
    }
    out->vertex_count = static_cast<int>(vertices);
    out->triangle_count = static_cast<int>(out->indices.size() / 3);
}

void Range(const std::vector<double> &values, const RenderOptions &options, double *low,
           double *high) {
    if (!options.auto_range) {
        *low = options.range_min;
        *high = options.range_max;
        return;
    }
    *low = 0.0;
    *high = 0.0;
    bool first = true;
    for (const double value : values) {
        if (!std::isfinite(value)) continue;
        if (first) {
            *low = *high = value;
            first = false;
            continue;
        }
        *low = std::min(*low, value);
        *high = std::max(*high, value);
    }
}

double ChooseScale(const AnalysisModel &model, const std::vector<Vec3d> &displacement,
                   bool auto_scale, double fraction, double fixed) {
    if (!auto_scale) return fixed;
    Vec3d low = model.nodes.empty() ? Vec3d{} : model.nodes[0];
    Vec3d high = low;
    for (const Vec3d &p : model.nodes) {
        low = Vec3d{std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
        high = Vec3d{std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
    }
    double largest = 0.0;
    for (const Vec3d &u : displacement) largest = std::max(largest, u.Length());
    const double diagonal = (high - low).Length();
    if (!(largest > 0.0) || !(diagonal > 0.0)) return 1.0;
    return fraction * diagonal / largest;
}

std::vector<Vec3d> Deformed(const AnalysisModel &model, const std::vector<Vec3d> &displacement,
                            double scale) {
    std::vector<Vec3d> out(model.nodes.size());
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        out[i] = model.nodes[i] + (i < displacement.size() ? displacement[i] * scale : Vec3d{});
    }
    return out;
}

// --- Marching tetrahedra ---------------------------------------------------
//
// One tetrahedron, four nodal values, one level. THE WHOLE OF THE
// SUBJECT IS THAT THERE IS NO AMBIGUOUS CASE: with one vertex on one side
// the surface is a triangle, with two on each side it is a quadrilateral,
// and neither admits a second topology to choose wrongly. The vertices of
// the polygon lie on edges, at a position linear in the values, so a
// shared edge produces the same point in both tets that own it and the
// result is watertight by construction rather than by care.
template <typename Emit>
void MarchTet(const Vec3d corner[4], const double value[4], const double colour[4], double level,
              Emit emit) {
    int below[4];
    int above[4];
    int below_count = 0;
    int above_count = 0;
    for (int i = 0; i < 4; ++i) {
        if (value[i] < level) {
            below[below_count++] = i;
        } else {
            above[above_count++] = i;
        }
    }
    if (below_count == 0 || above_count == 0) return;
    auto cross = [&](int a, int b, Vec3d *position, double *shade) {
        const double span = value[b] - value[a];
        const double t = span != 0.0 ? (level - value[a]) / span : 0.5;
        *position = corner[a] + (corner[b] - corner[a]) * t;
        *shade = colour[a] + (colour[b] - colour[a]) * t;
    };
    Vec3d p[4];
    double s[4];
    // WOUND TOWARDS INCREASING FIELD, EVERY TIME. Without this the
    // triangles come out with whatever orientation the case analysis
    // happened to give, which is invisible in a picture with two-sided
    // lighting and makes the enclosed volume meaningless -- the sphere
    // below reported 0.044 against its true 0.113 while its area was
    // right to a fraction of a percent. The direction from a vertex
    // below the level to one above it is the direction the surface
    // normal must have, and that needs no case analysis at all.
    const Vec3d outward = corner[above[0]] - corner[below[0]];
    auto orient = [&](int count) {
        if ((p[1] - p[0]).Cross(p[2] - p[0]).Dot(outward) >= 0.0) return;
        std::swap(p[0], p[count - 1]);
        std::swap(s[0], s[count - 1]);
        if (count == 4) {
            std::swap(p[1], p[2]);
            std::swap(s[1], s[2]);
        }
    };
    if (below_count == 1 || above_count == 1) {
        const int alone = below_count == 1 ? below[0] : above[0];
        const int *others = below_count == 1 ? above : below;
        for (int i = 0; i < 3; ++i) cross(alone, others[i], &p[i], &s[i]);
        orient(3);
        emit(p, s, 3);
        return;
    }
    // Two and two: the quadrilateral's corners must go round it rather
    // than criss-cross, so the edges are listed in an order that walks
    // the loop -- below0-above0, above0-below1, below1-above1,
    // above1-below0.
    cross(below[0], above[0], &p[0], &s[0]);
    cross(below[1], above[0], &p[1], &s[1]);
    cross(below[1], above[1], &p[2], &s[2]);
    cross(below[0], above[1], &p[3], &s[3]);
    orient(4);
    emit(p, s, 4);
}

}  // namespace

bool Decompose(const AnalysisModel &model, TetView *out, std::string *error) {
    *out = TetView{};
    for (int e = 0; e < model.ElementCount(); ++e) {
        const BoundElement &element = model.elements[Idx(e)];
        const std::vector<int> &n = element.nodes;
        auto add = [&](int a, int b, int c, int d) {
            std::array<int, 4> tet{n[Idx(a)], n[Idx(b)], n[Idx(c)], n[Idx(d)]};
            // Wound so that the volume is positive, always. Everything
            // downstream -- the outward face test, the marching cases --
            // is easier to reason about when that is guaranteed here once
            // than when each caller has to allow for either.
            if (Volume(model.nodes[Idx(tet[0])], model.nodes[Idx(tet[1])],
                       model.nodes[Idx(tet[2])], model.nodes[Idx(tet[3])]) < 0.0) {
                std::swap(tet[1], tet[2]);
            }
            out->tets.push_back(tet);
            out->element_of_tet.push_back(e);
        };
        switch (element.shape) {
            case ElementShape::Tet4: add(0, 1, 2, 3); break;
            case ElementShape::Tet10:
                for (const auto &tet : kTet10Split) add(tet[0], tet[1], tet[2], tet[3]);
                break;
            case ElementShape::Hex8:
            case ElementShape::Hex20:
                for (const auto &tet : kHexSplit) add(tet[0], tet[1], tet[2], tet[3]);
                break;
            case ElementShape::Wedge6:
            case ElementShape::Wedge15:
                for (const auto &tet : kWedgeSplit) add(tet[0], tet[1], tet[2], tet[3]);
                break;
            case ElementShape::Pyr5:
                for (const auto &tet : kPyramidSplit) add(tet[0], tet[1], tet[2], tet[3]);
                break;
            default:
                *error = std::string("cannot draw a ") + ElementShapeName(element.shape) +
                         ": it has no volume to decompose";
                return false;
        }
    }
    return true;
}

bool BoundaryTriangles(const AnalysisModel &model, const TetView &view,
                       std::vector<std::array<int, 3>> *out, std::string *error) {
    out->clear();
    (void)error;
    static const int kFaces[4][3] = {{1, 2, 3}, {0, 3, 2}, {0, 1, 3}, {0, 2, 1}};
    // Each face keyed by its sorted vertices, carrying how many tets
    // claimed it and, for the first one, its three vertices followed by
    // the tet's fourth -- the vertex opposite the face, which is what
    // says which way is out and which the second pass has no other way
    // to find.
    std::map<std::array<int, 3>, std::pair<int, std::array<int, 4>>> seen;
    for (const std::array<int, 4> &tet : view.tets) {
        for (int f = 0; f < 4; ++f) {
            const std::array<int, 4> face{tet[Idx(kFaces[f][0])], tet[Idx(kFaces[f][1])],
                                          tet[Idx(kFaces[f][2])], tet[Idx(f)]};
            std::array<int, 3> key{face[0], face[1], face[2]};
            std::sort(key.begin(), key.end());
            const auto found = seen.find(key);
            if (found == seen.end()) {
                seen.emplace(key, std::make_pair(1, face));
            } else {
                ++found->second.first;
            }
        }
    }
    for (const auto &entry : seen) {
        if (entry.second.first != 1) continue;
        const std::array<int, 4> &face = entry.second.second;
        const Vec3d a = model.nodes[Idx(face[0])];
        const Vec3d b = model.nodes[Idx(face[1])];
        const Vec3d c = model.nodes[Idx(face[2])];
        const Vec3d inside = model.nodes[Idx(face[3])];
        std::array<int, 3> triangle{face[0], face[1], face[2]};
        // FLIPPED BY TEST RATHER THAN BY CONVENTION. Working out which
        // winding a face table gives for a tet of known orientation is
        // the kind of reasoning that is right four times out of five, and
        // the fifth shows up as a black patch in a render. Pointing the
        // normal away from the vertex that is definitely inside cannot be
        // got wrong.
        if ((b - a).Cross(c - a).Dot(a - inside) < 0.0) std::swap(triangle[1], triangle[2]);
        out->push_back(triangle);
    }
    return true;
}

bool RenderSurface(const AnalysisModel &model, const std::vector<Vec3d> &displacement,
                   const std::vector<double> &values, const RenderOptions &options,
                   RenderMesh *out, std::string *error) {
    *out = RenderMesh{};
    if (values.size() != model.nodes.size()) {
        *error = "the field is not one value per node";
        return false;
    }
    TetView view;
    if (!Decompose(model, &view, error)) return false;
    std::vector<std::array<int, 3>> triangles;
    if (!BoundaryTriangles(model, view, &triangles, error)) return false;

    const double scale =
        options.undeformed ? 0.0
                           : ChooseScale(model, displacement, options.auto_scale,
                                         options.auto_scale_fraction, options.displacement_scale);
    const std::vector<Vec3d> moved = Deformed(model, displacement, scale);
    out->scale_used = scale;
    Builder build;
    build.out = out;
    build.map = options.color_map;
    Range(values, options, &build.low, &build.high);
    out->field_min = build.low;
    out->field_max = build.high;

    const Vec3d normal =
        options.clip_normal.LengthSquared() > 0.0 ? options.clip_normal.Normalized() : Vec3d{1, 0, 0};
    // One vertex per node, shared between the triangles that meet there,
    // so the colour and the normal interpolate smoothly across the
    // surface. De-indexing -- giving every triangle its own three
    // vertices, as every importer in this tree used to do to dodge a
    // 16-bit index cap -- would turn a smooth contour into a faceted one.
    std::vector<unsigned int> index_of_node(model.nodes.size(), 0xFFFFFFFFu);
    for (const std::array<int, 3> &triangle : triangles) {
        if (options.clip) {
            // WHOLLY ON THE KEPT SIDE, not merely touching it. Keeping
            // every triangle with one vertex across leaves a slab of the
            // discarded half attached -- on a cube cut down the middle,
            // an extra ring of area one. Triangles are not cut, so the
            // exposed edge is ragged wherever the mesh does not line up
            // with the plane; the clean flat face is what RenderSection
            // is for, and the two are meant to be drawn together.
            bool keep = true;
            for (const int node : triangle) {
                if ((model.nodes[Idx(node)] - options.clip_point).Dot(normal) < 0.0) keep = false;
            }
            if (!keep) continue;
        }
        unsigned int corner[3];
        for (int i = 0; i < 3; ++i) {
            const int node = triangle[Idx(i)];
            if (index_of_node[Idx(node)] == 0xFFFFFFFFu) {
                index_of_node[Idx(node)] = build.Vertex(moved[Idx(node)], values[Idx(node)]);
            }
            corner[i] = index_of_node[Idx(node)];
        }
        build.Triangle(corner[0], corner[1], corner[2]);
    }
    FinishNormals(out, build.exact);
    return true;
}

namespace {

bool RenderLevelSet(const AnalysisModel &model, const std::vector<Vec3d> &displacement,
                    const std::vector<double> &values, double level,
                    const std::vector<double> &colour_by, const RenderOptions &options,
                    RenderMesh *out, std::string *error) {
    *out = RenderMesh{};
    if (values.size() != model.nodes.size()) {
        *error = "the field is not one value per node";
        return false;
    }
    const std::vector<double> &shade = colour_by.empty() ? values : colour_by;
    if (shade.size() != model.nodes.size()) {
        *error = "the colouring field is not one value per node";
        return false;
    }
    TetView view;
    if (!Decompose(model, &view, error)) return false;
    const double scale =
        options.undeformed ? 0.0
                           : ChooseScale(model, displacement, options.auto_scale,
                                         options.auto_scale_fraction, options.displacement_scale);
    const std::vector<Vec3d> moved = Deformed(model, displacement, scale);
    out->scale_used = scale;
    Builder build;
    build.out = out;
    build.map = options.color_map;
    Range(shade, options, &build.low, &build.high);
    out->field_min = build.low;
    out->field_max = build.high;

    for (const std::array<int, 4> &tet : view.tets) {
        Vec3d corner[4];
        double value[4];
        double colour[4];
        for (int i = 0; i < 4; ++i) {
            corner[i] = moved[Idx(tet[Idx(i)])];
            value[i] = values[Idx(tet[Idx(i)])];
            colour[i] = shade[Idx(tet[Idx(i)])];
        }
        MarchTet(corner, value, colour, level, [&](const Vec3d *p, const double *s, int count) {
            unsigned int index[4];
            for (int i = 0; i < count; ++i) index[i] = build.Vertex(p[i], s[i]);
            build.Triangle(index[0], index[1], index[2]);
            if (count == 4) build.Triangle(index[0], index[2], index[3]);
        });
    }
    FinishNormals(out, build.exact);
    return true;
}

}  // namespace

bool RenderIsosurface(const AnalysisModel &model, const std::vector<Vec3d> &displacement,
                      const std::vector<double> &values, double level,
                      const std::vector<double> &colour_by, const RenderOptions &options,
                      RenderMesh *out, std::string *error) {
    return RenderLevelSet(model, displacement, values, level, colour_by, options, out, error);
}

bool RenderSection(const AnalysisModel &model, const std::vector<Vec3d> &displacement,
                   const std::vector<double> &values, const Vec3d &point, const Vec3d &normal,
                   const RenderOptions &options, RenderMesh *out, std::string *error) {
    if (normal.LengthSquared() <= 0.0) {
        *error = "the cutting plane has no normal";
        return false;
    }
    // A SECTION IS AN ISOSURFACE OF THE SIGNED DISTANCE TO THE PLANE, at
    // level zero. Writing it any other way would be writing the
    // polygon-through-a-tetrahedron code a second time, and the second
    // copy is where the two would come to disagree about a degenerate
    // case.
    const Vec3d unit = normal.Normalized();
    std::vector<double> distance(model.nodes.size(), 0.0);
    for (std::size_t i = 0; i < model.nodes.size(); ++i) {
        distance[i] = (model.nodes[i] - point).Dot(unit);
    }
    return RenderLevelSet(model, displacement, distance, 0.0, values, options, out, error);
}

bool RenderVectors(const AnalysisModel &model, const std::vector<Vec3d> &at,
                   const std::vector<Vec3d> &vectors, const VectorOptions &options,
                   RenderMesh *out, std::string *error) {
    *out = RenderMesh{};
    if (at.size() != vectors.size()) {
        *error = "there is not one position per vector";
        return false;
    }
    const int stride = std::max(1, options.stride);
    double largest = 0.0;
    for (const Vec3d &v : vectors) largest = std::max(largest, v.Length());
    double scale = options.scale;
    if (options.auto_scale) {
        Vec3d low = model.nodes.empty() ? Vec3d{} : model.nodes[0];
        Vec3d high = low;
        for (const Vec3d &p : model.nodes) {
            low = Vec3d{std::min(low.x, p.x), std::min(low.y, p.y), std::min(low.z, p.z)};
            high = Vec3d{std::max(high.x, p.x), std::max(high.y, p.y), std::max(high.z, p.z)};
        }
        const double diagonal = (high - low).Length();
        scale = largest > 0.0 && diagonal > 0.0 ? options.auto_scale_fraction * diagonal / largest
                                                : 1.0;
    }
    out->scale_used = scale;
    std::vector<double> magnitude;
    for (const Vec3d &v : vectors) magnitude.push_back(v.Length());
    Builder build;
    build.out = out;
    build.map = options.color_map;
    if (options.auto_range) {
        RenderOptions range_options;
        Range(magnitude, range_options, &build.low, &build.high);
    } else {
        build.low = options.range_min;
        build.high = options.range_max;
    }
    out->field_min = build.low;
    out->field_max = build.high;

    for (std::size_t i = 0; i < at.size(); i += static_cast<std::size_t>(stride)) {
        const Vec3d tail = at[i];
        const Vec3d arrow = vectors[i] * scale;
        if (!(arrow.LengthSquared() > 0.0)) continue;
        const Vec3d tip = tail + arrow;
        const unsigned int a = build.Vertex(tail, magnitude[i]);
        const unsigned int b = build.Vertex(tip, magnitude[i]);
        out->line_indices.push_back(a);
        out->line_indices.push_back(b);
        // A head made of two barbs in a plane containing the shaft. Two
        // is enough to read the direction from any angle that is not
        // exactly along the shaft, and it costs four vertices rather than
        // the dozens a cone would.
        Vec3d sideways = arrow.Cross(Vec3d{0, 0, 1});
        if (sideways.LengthSquared() < arrow.LengthSquared() * 1e-12) {
            sideways = arrow.Cross(Vec3d{0, 1, 0});
        }
        if (sideways.LengthSquared() <= 0.0) continue;
        sideways = sideways.Normalized() * (arrow.Length() * options.head_fraction * 0.5);
        const Vec3d back = tip - arrow * options.head_fraction;
        for (const Vec3d &barb : {back + sideways, back - sideways}) {
            const unsigned int c = build.Vertex(barb, magnitude[i]);
            out->line_indices.push_back(b);
            out->line_indices.push_back(c);
        }
    }
    out->vertex_count = static_cast<int>(out->positions.size() / 3);
    out->normals.assign(out->positions.size(), 0.0f);
    return true;
}

}  // namespace fem
