#include "cad_drawing.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <map>
#include <tuple>
#include <utility>

namespace cad {
namespace {

// A ray/triangle intersection, Moller-Trumbore. Used to ask "is there
// material between this point and the viewer", which is the only question
// hidden-line removal asks.
bool RayHitsTriangle(const Vec3d &origin, const Vec3d &direction, const Vec3d &a, const Vec3d &b,
                     const Vec3d &c, double *out_t) {
    const Vec3d edge1 = b - a;
    const Vec3d edge2 = c - a;
    const Vec3d h = direction.Cross(edge2);
    const double determinant = edge1.Dot(h);
    if (std::fabs(determinant) < 1e-15) return false;
    const double inverse = 1.0 / determinant;
    const Vec3d s = origin - a;
    const double u = inverse * s.Dot(h);
    if (u < 0.0 || u > 1.0) return false;
    const Vec3d q = s.Cross(edge1);
    const double v = inverse * direction.Dot(q);
    if (v < 0.0 || u + v > 1.0) return false;
    const double t = inverse * edge2.Dot(q);
    if (t <= 0.0) return false;
    *out_t = t;
    return true;
}

// A uniform grid over the projected triangles, so that a point is tested
// against the few triangles that could cover it rather than all of them.
// Without it a drawing of anything real is quadratic and unusably slow.
class ProjectedIndex {
public:
    void Build(const TessellationMesh &mesh, const Vec3d &right, const Vec3d &up) {
        mesh_ = &mesh;
        const std::size_t triangles = static_cast<std::size_t>(mesh.TriangleCount());
        boxes_.resize(triangles);
        for (std::size_t t = 0; t < triangles; ++t) {
            Interval x;
            Interval y;
            for (int k = 0; k < 3; ++k) {
                const Vec3d &p = mesh.positions[static_cast<std::size_t>(
                    mesh.indices[t * 3 + static_cast<std::size_t>(k)])];
                x.Expand(p.Dot(right));
                y.Expand(p.Dot(up));
            }
            boxes_[t] = {x, y};
            extent_x_.Expand(x.lo);
            extent_x_.Expand(x.hi);
            extent_y_.Expand(y.lo);
            extent_y_.Expand(y.hi);
        }
        if (triangles == 0) return;
        cells_ = std::max(1, static_cast<int>(std::sqrt(static_cast<double>(triangles)) / 2.0));
        buckets_.assign(static_cast<std::size_t>(cells_ * cells_), {});
        for (std::size_t t = 0; t < triangles; ++t) {
            int x0 = 0;
            int x1 = 0;
            int y0 = 0;
            int y1 = 0;
            Cells(boxes_[t].first, boxes_[t].second, &x0, &x1, &y0, &y1);
            for (int y = y0; y <= y1; ++y) {
                for (int x = x0; x <= x1; ++x) {
                    buckets_[static_cast<std::size_t>(y * cells_ + x)].push_back(t);
                }
            }
        }
    }

    // Is `point` behind any triangle, looking along `direction`?
    bool Occluded(const Vec3d &point, const Vec3d &direction, const Vec3d &right, const Vec3d &up,
                  double bias) const {
        if (mesh_ == nullptr || cells_ == 0) return false;
        const double px = point.Dot(right);
        const double py = point.Dot(up);
        Interval x;
        x.Expand(px);
        Interval y;
        y.Expand(py);
        int x0 = 0;
        int x1 = 0;
        int y0 = 0;
        int y1 = 0;
        Cells(x, y, &x0, &x1, &y0, &y1);
        // The ray runs *backwards* along the view direction: from the
        // point towards the viewer. Anything it hits is in front.
        const Vec3d towards_viewer = direction * -1.0;
        const Vec3d origin = point + towards_viewer * bias;
        for (int cy = y0; cy <= y1; ++cy) {
            for (int cx = x0; cx <= x1; ++cx) {
                for (std::size_t t : buckets_[static_cast<std::size_t>(cy * cells_ + cx)]) {
                    if (px < boxes_[t].first.lo || px > boxes_[t].first.hi) continue;
                    if (py < boxes_[t].second.lo || py > boxes_[t].second.hi) continue;
                    const Vec3d &a = mesh_->positions[static_cast<std::size_t>(mesh_->indices[t * 3])];
                    const Vec3d &b =
                        mesh_->positions[static_cast<std::size_t>(mesh_->indices[t * 3 + 1])];
                    const Vec3d &c =
                        mesh_->positions[static_cast<std::size_t>(mesh_->indices[t * 3 + 2])];
                    double distance = 0.0;
                    if (RayHitsTriangle(origin, towards_viewer, a, b, c, &distance)) return true;
                }
            }
        }
        return false;
    }

private:
    void Cells(const Interval &x, const Interval &y, int *x0, int *x1, int *y0, int *y1) const {
        const double width = std::max(1e-12, extent_x_.hi - extent_x_.lo);
        const double height = std::max(1e-12, extent_y_.hi - extent_y_.lo);
        auto clamp = [this](int v) { return std::max(0, std::min(cells_ - 1, v)); };
        *x0 = clamp(static_cast<int>((x.lo - extent_x_.lo) / width * cells_));
        *x1 = clamp(static_cast<int>((x.hi - extent_x_.lo) / width * cells_));
        *y0 = clamp(static_cast<int>((y.lo - extent_y_.lo) / height * cells_));
        *y1 = clamp(static_cast<int>((y.hi - extent_y_.lo) / height * cells_));
    }

    const TessellationMesh *mesh_ = nullptr;
    std::vector<std::pair<Interval, Interval>> boxes_;
    std::vector<std::vector<std::size_t>> buckets_;
    Interval extent_x_ = Interval::Empty();
    Interval extent_y_ = Interval::Empty();
    int cells_ = 0;
};

}  // namespace

bool MakeDrawingView(const Model &model, const std::vector<EntityId> &bodies,
                     const DrawingViewOptions &options, DrawingView *out, std::string *error) {
    error->clear();
    *out = DrawingView{};
    if (bodies.empty()) {
        *error = "there are no bodies to draw";
        return false;
    }
    const double length = options.direction.Length();
    if (!(length > 0.0)) {
        *error = "the view direction has no direction";
        return false;
    }
    const Vec3d forward = options.direction * (1.0 / length);
    Vec3d up = options.up - forward * forward.Dot(options.up);
    if (!(up.Length() > 1e-12)) up = forward.AnyPerpendicular();
    up = up.Normalized();
    const Vec3d right = up.Cross(forward).Normalized();

    // One mesh for the whole view: hiding is a question about the scene,
    // not about each body separately, so a body behind another has to be
    // hidden by it.
    TessellationMesh mesh;
    for (EntityId body : bodies) {
        TessellationMesh one;
        if (!TessellateBody(model, body, options.tessellation, &one, error)) return false;
        const int base = mesh.VertexCount();
        for (const Vec3d &p : one.positions) mesh.positions.push_back(p);
        for (const Vec3d &n : one.normals) mesh.normals.push_back(n);
        for (int index : one.indices) mesh.indices.push_back(base + index);
        for (EntityId face : one.triangle_face) mesh.triangle_face.push_back(face);
    }
    Box3d extent;
    for (const Vec3d &p : mesh.positions) extent.Expand(p);
    const double size = extent.IsEmpty()
                            ? 1.0
                            : std::max({extent.x.Width(), extent.y.Width(), extent.z.Width(), 1e-9});
    const double bias = size * options.relative_bias;

    ProjectedIndex index;
    index.Build(mesh, right, up);

    auto project = [&](const Vec3d &p) { return Vec2d{p.Dot(right), p.Dot(up)}; };
    auto add = [&](const Vec3d &a, const Vec3d &b, bool hidden, EntityId edge) {
        DrawingSegment segment;
        segment.a = project(a);
        segment.b = project(b);
        // An edge pointing straight at the viewer projects to a point,
        // and a drawing does not draw those. A cube seen square on has
        // four of them -- the ones running away from you -- and leaving
        // them in gives a view with twelve lines in it, four of which are
        // invisible and all of which confuse anything counting.
        if ((segment.b - segment.a).Length() <= size * 1e-9) return;
        segment.hidden = hidden;
        segment.edge = edge;
        out->x.Expand(segment.a.x);
        out->x.Expand(segment.b.x);
        out->y.Expand(segment.a.y);
        out->y.Expand(segment.b.y);
        if (hidden) {
            ++out->hidden;
        } else {
            ++out->visible;
        }
        out->segments.push_back(segment);
    };

    // The edges, cut into pieces, each piece kept whole and marked by
    // where its middle is. Consecutive pieces of the same visibility are
    // joined back up, so a fully visible edge is one segment again rather
    // than sixty-four.
    const int pieces = std::max(1, options.pieces_per_edge);
    std::vector<EntityId> seen;
    for (EntityId body : bodies) {
        const Body *solid = model.GetBody(body);
        if (solid == nullptr) continue;
        for (EntityId shell : solid->shells) {
            for (EntityId edge_id : model.EdgesOfShell(shell)) {
                if (std::find(seen.begin(), seen.end(), edge_id) != seen.end()) continue;
                seen.push_back(edge_id);
                const Edge *edge = model.GetEdge(edge_id);
                const Curve3 *curve = model.CurveAt(edge->curve);
                if (curve == nullptr) continue;
                Vec3d run_start = curve->Point(edge->t_start);
                bool run_hidden = false;
                bool have_run = false;
                for (int i = 0; i < pieces; ++i) {
                    const double t0 = edge->t_start + (edge->t_end - edge->t_start) *
                                                          static_cast<double>(i) /
                                                          static_cast<double>(pieces);
                    const double t1 = edge->t_start + (edge->t_end - edge->t_start) *
                                                          static_cast<double>(i + 1) /
                                                          static_cast<double>(pieces);
                    const Vec3d a = curve->Point(t0);
                    const Vec3d b = curve->Point(t1);
                    const bool hidden =
                        index.Occluded((a + b) * 0.5, forward, right, up, bias);
                    if (!have_run) {
                        run_start = a;
                        run_hidden = hidden;
                        have_run = true;
                    } else if (hidden != run_hidden) {
                        add(run_start, a, run_hidden, edge_id);
                        run_start = a;
                        run_hidden = hidden;
                    }
                    if (i + 1 == pieces) add(run_start, b, run_hidden, edge_id);
                }
            }
        }
    }

    // Silhouettes: where a curved face turns away from the viewer there is
    // an outline that is not an edge of the body at all. A cylinder seen
    // from the side is two straight lines and neither is in its topology.
    // They are found on the mesh, as the triangle edges whose two
    // triangles face opposite ways.
    if (options.silhouettes) {
        std::map<std::pair<long long, long long>, std::pair<int, int>> shared;
        auto key_of = [](const Vec3d &p) {
            return std::make_pair(static_cast<long long>(std::llround(p.x * 1e7)),
                                  static_cast<long long>(std::llround(p.y * 1e7)));
        };
        (void)key_of;
        const std::size_t triangles = static_cast<std::size_t>(mesh.TriangleCount());
        std::map<std::pair<int, int>, std::vector<std::size_t>> edges;
        // Weld by position, since each face's vertices are its own.
        std::map<std::tuple<long long, long long, long long>, int> welded;
        std::vector<int> vertex_key(mesh.positions.size(), 0);
        for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
            const auto key = std::make_tuple(std::llround(mesh.positions[i].x * 1e7),
                                             std::llround(mesh.positions[i].y * 1e7),
                                             std::llround(mesh.positions[i].z * 1e7));
            const auto found = welded.find(key);
            if (found == welded.end()) {
                const int next = static_cast<int>(welded.size());
                welded.emplace(key, next);
                vertex_key[i] = next;
            } else {
                vertex_key[i] = found->second;
            }
        }
        for (std::size_t t = 0; t < triangles; ++t) {
            for (int k = 0; k < 3; ++k) {
                int a = vertex_key[static_cast<std::size_t>(mesh.indices[t * 3 + static_cast<std::size_t>(k)])];
                int b = vertex_key[static_cast<std::size_t>(
                    mesh.indices[t * 3 + static_cast<std::size_t>((k + 1) % 3)])];
                if (a > b) std::swap(a, b);
                edges[{a, b}].push_back(t);
            }
        }
        for (const auto &entry : edges) {
            if (entry.second.size() != 2) continue;
            auto facing = [&](std::size_t t) {
                const Vec3d &a = mesh.positions[static_cast<std::size_t>(mesh.indices[t * 3])];
                const Vec3d &b = mesh.positions[static_cast<std::size_t>(mesh.indices[t * 3 + 1])];
                const Vec3d &c = mesh.positions[static_cast<std::size_t>(mesh.indices[t * 3 + 2])];
                return (b - a).Cross(c - a).Dot(forward);
            };
            const double first = facing(entry.second[0]);
            const double second = facing(entry.second[1]);
            if (first == 0.0 || second == 0.0) continue;
            if ((first > 0.0) == (second > 0.0)) continue;
            // A silhouette edge. Find its two endpoints again.
            Vec3d a;
            Vec3d b;
            bool have_a = false;
            for (std::size_t i = 0; i < mesh.positions.size(); ++i) {
                if (vertex_key[i] == entry.first.first && !have_a) {
                    a = mesh.positions[i];
                    have_a = true;
                }
                if (vertex_key[i] == entry.first.second) b = mesh.positions[i];
            }
            const Vec3d middle = (a + b) * 0.5;
            add(a, b, index.Occluded(middle, forward, right, up, bias), kNoEntity);
        }
    }
    return true;
}

std::string DrawingViewToSvg(const DrawingView &view, double width_mm, double margin_mm) {
    const double width = std::max(1e-9, view.x.hi - view.x.lo);
    const double height = std::max(1e-9, view.y.hi - view.y.lo);
    const double scale = (width_mm - 2.0 * margin_mm) / width;
    const double sheet_height = height * scale + 2.0 * margin_mm;
    char buffer[256];
    std::snprintf(buffer, sizeof(buffer),
                  "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.3fmm\" height=\"%.3fmm\" "
                  "viewBox=\"0 0 %.3f %.3f\">\n",
                  width_mm, sheet_height, width_mm, sheet_height);
    std::string out = buffer;
    out += "<rect width=\"100%\" height=\"100%\" fill=\"white\"/>\n";
    // SVG's y runs down the page and a drawing's runs up it.
    auto place = [&](const Vec2d &p) {
        return Vec2d{margin_mm + (p.x - view.x.lo) * scale,
                     sheet_height - margin_mm - (p.y - view.y.lo) * scale};
    };
    for (const DrawingSegment &segment : view.segments) {
        const Vec2d a = place(segment.a);
        const Vec2d b = place(segment.b);
        std::snprintf(buffer, sizeof(buffer),
                      "<line x1=\"%.4f\" y1=\"%.4f\" x2=\"%.4f\" y2=\"%.4f\" stroke=\"black\" "
                      "stroke-width=\"%.3f\"%s/>\n",
                      a.x, a.y, b.x, b.y, segment.hidden ? 0.18 : 0.35,
                      segment.hidden ? " stroke-dasharray=\"1.2 0.8\"" : "");
        out += buffer;
    }
    out += "</svg>\n";
    return out;
}

}  // namespace cad
