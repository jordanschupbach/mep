// Surface meshing in parameter space (plans/CAD_FEM_PLAN.md Part G.2).
//
// WHY THIS CANNOT BE A GENERIC 2D MESHER. A face is meshed in its own
// (u, v), because that is where its boundary is a polygon and its
// interior is simply connected. But an element that is well-shaped in
// (u, v) is not well-shaped in space: a cylinder's parameters are an
// angle and a height, so a square in parameter space is a rectangle of
// aspect ratio r on the surface. Every length and angle below is
// therefore measured under the metric the surface induces on its
// parameters -- the first fundamental form -- and that is the whole
// difference between this and a mesher that happens to run on a
// rectangle.
//
// SHARED EDGES ARE MESHED ONCE. Two faces meeting along an edge get the
// *same* nodes on it, which is what makes the result a closed surface
// rather than a pile of patches that nearly line up. A per-face mesher
// that does not do this is the most common way to produce a surface mesh
// with cracks in it that no amount of tolerance will close.
//
// The refinement is Ruppert's: triangulate the boundary, then repeatedly
// split the worst triangle at its circumcentre, except that a point which
// would encroach on a boundary segment splits that segment instead. That
// exception is what keeps the boundary in the triangulation without a
// separate recovery step, and it is why the minimum angle is capped at
// 25 degrees below -- past about 20.7 the method can cycle, and that is a
// property of the algorithm rather than a tolerance to tune.

#include "cad_predicates.h"
#include "fem_mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <utility>

namespace fem {
namespace {

using cad::Vec2d;
using cad::Vec3d;

// The metric at a parameter point: lengths and angles in (u, v) measured
// as the surface sees them.
struct Metric {
    double e = 1.0, f = 0.0, g = 1.0;
    // The 3D length of a step in parameter space.
    double Length(const Vec2d &d) const {
        return std::sqrt(std::max(0.0, e * d.x * d.x + 2.0 * f * d.x * d.y + g * d.y * d.y));
    }
};

Metric MetricAt(const cad::Surface &surface, const Vec2d &uv) {
    Metric m;
    surface.FirstFundamentalForm(uv.x, uv.y, &m.e, &m.f, &m.g);
    if (!(m.e > 0.0)) m.e = 1.0;
    if (!(m.g > 0.0)) m.g = 1.0;
    return m;
}

// The 3D length of the segment ab, measured through the metric at its
// two ends. Averaging the two is a midpoint rule on the arc length
// integral and is exact for a metric that varies linearly.
double MetricLength(const cad::Surface &surface, const Vec2d &a, const Vec2d &b) {
    const Vec2d d{b.x - a.x, b.y - a.y};
    return 0.5 * (MetricAt(surface, a).Length(d) + MetricAt(surface, b).Length(d));
}

struct Tri {
    int a = 0, b = 0, c = 0;
    bool alive = true;
};

std::array<double, 2> Coord(const std::vector<Vec2d> &points, int i) {
    return {points[static_cast<std::size_t>(i)].x, points[static_cast<std::size_t>(i)].y};
}

// Delaunay by Bowyer-Watson with a vertex at infinity, exactly as
// cad_tessellate.cpp does it and for the reason recorded there: a super
// triangle cannot be made big enough, because it is the circumcircles
// that have to be contained and a sliver's is unbounded.
void Triangulate(const std::vector<Vec2d> &points, std::vector<Tri> *out) {
    out->clear();
    const int count = static_cast<int>(points.size());
    if (count < 3) return;
    const int infinity = count;

    int second = -1;
    for (int i = 1; i < count && second < 0; ++i) {
        if (points[static_cast<std::size_t>(i)].x != points[0].x ||
            points[static_cast<std::size_t>(i)].y != points[0].y) {
            second = i;
        }
    }
    if (second < 0) return;
    int third = -1;
    for (int i = second + 1; i < count && third < 0; ++i) {
        if (cad::Orient2D(Coord(points, 0).data(), Coord(points, second).data(),
                          Coord(points, i).data()) != 0) {
            third = i;
        }
    }
    if (third < 0) return;

    std::vector<Tri> tris;
    Tri seed{0, second, third, true};
    if (cad::Orient2D(Coord(points, seed.a).data(), Coord(points, seed.b).data(),
                      Coord(points, seed.c).data()) < 0) {
        std::swap(seed.b, seed.c);
    }
    tris.push_back(seed);
    tris.push_back(Tri{seed.b, seed.a, infinity, true});
    tris.push_back(Tri{seed.c, seed.b, infinity, true});
    tris.push_back(Tri{seed.a, seed.c, infinity, true});

    // TIES ARE BROKEN SYMBOLICALLY, THE SAME WAY PART G.3 BREAKS THEM.
    // Exactly cocircular points are common on the faces a mesher meets --
    // a lattice of them on a planar face is cocircular four at a time --
    // and "InCircle returned zero, so no conflict" is not a consistent
    // rule: the cavity a point digs stops being star-shaped and the
    // triangulation is refilled with whatever the round-off suggested.
    // Lifting point i onto the paraboloid by e^i more than its neighbours
    // is a regular triangulation with weights too small to hide any point
    // and it has no ties. InCircle is the determinant of the lifted
    // points, so raising one lift moves it by the cofactor of that entry,
    // which is up to sign an Orient2D of the other three; the signs
    // alternate and were measured against a direct evaluation of the
    // determinant rather than derived.
    //
    // THIS DOES NOT FIX THE SCALE-DEPENDENCE RECORDED IN THE PLAN, and it
    // was written while looking for it. A cylinder's cap has boundary
    // nodes that are cocircular in intent and not in fact: they come from
    // evaluating a circle at parameters, so they sit on it to within
    // round-off. The exact predicate then answers correctly for the points
    // it was actually given, the answer differs between one scale and
    // another because the round-off does, and there is no tie to break.
    // What is left is that a *nearly* degenerate point set has many valid
    // Delaunay triangulations and some of them contain slivers -- which is
    // the refinement pass's job to remove, and the note at the
    // encroachment test below says why it currently cannot.
    auto in_circle = [&](int a, int b, int c, int p) {
        const int v[4] = {a, b, c, p};
        std::array<std::array<double, 2>, 4> at{};
        for (int k = 0; k < 4; ++k) at[static_cast<std::size_t>(k)] = Coord(points, v[k]);
        const int s = cad::InCircle(at[0].data(), at[1].data(), at[2].data(), at[3].data());
        if (s != 0) return s > 0;
        int order[4] = {0, 1, 2, 3};
        for (int k = 1; k < 4; ++k) {
            for (int m = k; m > 0 && v[order[m]] < v[order[m - 1]]; --m) {
                std::swap(order[m], order[m - 1]);
            }
        }
        for (const int j : order) {
            const double *other[3];
            int filled = 0;
            for (int r = 0; r < 4; ++r) {
                if (r != j) other[filled++] = at[static_cast<std::size_t>(r)].data();
            }
            const int side = cad::Orient2D(other[0], other[1], other[2]);
            if (side == 0) continue;
            return (j % 2 == 0) ? side > 0 : side < 0;
        }
        // All four collinear: the determinant is identically zero and no
        // perturbation of the lift alone breaks it.
        return false;
    };

    auto conflicts = [&](const Tri &t, int p) {
        const std::array<double, 2> point = Coord(points, p);
        if (t.c != infinity) return in_circle(t.a, t.b, t.c, p);
        const std::array<double, 2> a = Coord(points, t.a);
        const std::array<double, 2> b = Coord(points, t.b);
        const int side = cad::Orient2D(a.data(), b.data(), point.data());
        if (side != 0) return side > 0;
        const double ux = b[0] - a[0];
        const double uy = b[1] - a[1];
        return (point[0] - a[0]) * ux + (point[1] - a[1]) * uy > 0.0 &&
               (point[0] - b[0]) * ux + (point[1] - b[1]) * uy < 0.0;
    };

    std::vector<std::pair<int, int>> cavity;
    std::size_t dead = 0;
    for (int i = 0; i < count; ++i) {
        if (i == 0 || i == second || i == third) continue;
        if (dead * 2 > tris.size()) {
            tris.erase(std::remove_if(tris.begin(), tris.end(), [](const Tri &t) { return !t.alive; }),
                       tris.end());
            dead = 0;
        }
        cavity.clear();
        for (Tri &t : tris) {
            if (!t.alive || !conflicts(t, i)) continue;
            t.alive = false;
            ++dead;
            cavity.push_back({t.a, t.b});
            cavity.push_back({t.b, t.c});
            cavity.push_back({t.c, t.a});
        }
        for (std::size_t e = 0; e < cavity.size(); ++e) {
            bool shared = false;
            for (std::size_t f = 0; f < cavity.size() && !shared; ++f) {
                if (e == f) continue;
                if (cavity[e].first == cavity[f].second && cavity[e].second == cavity[f].first) {
                    shared = true;
                }
            }
            if (shared) continue;
            const int from = cavity[e].first;
            const int to = cavity[e].second;
            if (from == infinity) {
                tris.push_back(Tri{to, i, infinity, true});
            } else if (to == infinity) {
                tris.push_back(Tri{i, from, infinity, true});
            } else {
                tris.push_back(Tri{from, to, i, true});
            }
        }
    }
    for (const Tri &t : tris) {
        if (t.alive && t.c != infinity) out->push_back(t);
    }
}

// Is `p` inside the polygon, by ray crossing? The boundary is a closed
// chain of segment indices into `points`.
bool InsidePolygon(const std::vector<Vec2d> &points, const std::vector<std::pair<int, int>> &segments,
                   const Vec2d &p) {
    bool inside = false;
    for (const auto &segment : segments) {
        const Vec2d &a = points[static_cast<std::size_t>(segment.first)];
        const Vec2d &b = points[static_cast<std::size_t>(segment.second)];
        if ((a.y > p.y) == (b.y > p.y)) continue;
        const double x = a.x + (p.y - a.y) / (b.y - a.y) * (b.x - a.x);
        if (p.x < x) inside = !inside;
    }
    return inside;
}

// Ruppert's encroachment: a point encroaches a segment when it lies
// inside the segment's diametral circle. Splitting the segment instead of
// inserting the point is what keeps the boundary in the triangulation.
bool Encroaches(const Vec2d &a, const Vec2d &b, const Vec2d &p) {
    const Vec2d centre{0.5 * (a.x + b.x), 0.5 * (a.y + b.y)};
    const double radius_squared = 0.25 * ((b.x - a.x) * (b.x - a.x) + (b.y - a.y) * (b.y - a.y));
    const double dx = p.x - centre.x;
    const double dy = p.y - centre.y;
    return dx * dx + dy * dy < radius_squared * (1.0 - 1e-12);
}

bool Circumcentre(const Vec2d &a, const Vec2d &b, const Vec2d &c, Vec2d *out) {
    const double ax = a.x - c.x;
    const double ay = a.y - c.y;
    const double bx = b.x - c.x;
    const double by = b.y - c.y;
    const double d = 2.0 * (ax * by - ay * bx);
    if (std::fabs(d) < 1e-300) return false;
    const double a2 = ax * ax + ay * ay;
    const double b2 = bx * bx + by * by;
    out->x = c.x + (by * a2 - ay * b2) / d;
    out->y = c.y + (ax * b2 - bx * a2) / d;
    return true;
}

// The smallest angle of a triangle, measured *through the metric* -- so
// what is reported is the angle the element has in space, not in
// parameter space.
double SmallestAngle(const cad::Surface &surface, const Vec2d &a, const Vec2d &b, const Vec2d &c) {
    const double ab = MetricLength(surface, a, b);
    const double bc = MetricLength(surface, b, c);
    const double ca = MetricLength(surface, c, a);
    if (!(ab > 0.0 && bc > 0.0 && ca > 0.0)) return 0.0;
    auto angle = [](double opposite, double x, double y) {
        const double cosine = (x * x + y * y - opposite * opposite) / (2.0 * x * y);
        return std::acos(std::max(-1.0, std::min(1.0, cosine)));
    };
    return std::min({angle(bc, ab, ca), angle(ca, ab, bc), angle(ab, bc, ca)});
}

}  // namespace

bool SurfaceMesh::IsClosed() const {
    std::map<std::pair<int, int>, int> directed;
    for (const SurfaceTriangle &t : triangles) {
        const int v[3] = {t.a, t.b, t.c};
        for (int k = 0; k < 3; ++k) ++directed[{v[k], v[(k + 1) % 3]}];
    }
    for (const auto &entry : directed) {
        if (entry.second != 1) return false;
        const auto reverse = directed.find({entry.first.second, entry.first.first});
        if (reverse == directed.end() || reverse->second != 1) return false;
    }
    return !directed.empty();
}

double SurfaceMesh::SignedVolume() const {
    double total = 0.0;
    for (const SurfaceTriangle &t : triangles) {
        const Vec3d &a = nodes[static_cast<std::size_t>(t.a)];
        const Vec3d &b = nodes[static_cast<std::size_t>(t.b)];
        const Vec3d &c = nodes[static_cast<std::size_t>(t.c)];
        total += a.Dot(b.Cross(c));
    }
    return total / 6.0;
}

bool MeshSurface(const cad::Model &model, const std::vector<cad::EntityId> &bodies,
                 const SurfaceMeshOptions &options, const SizingField &sizing, SurfaceMesh *out,
                 MeshReport *report) {
    *out = SurfaceMesh{};
    report->warnings.clear();
    report->error.clear();

    // --- Nodes shared between faces ----------------------------------------
    //
    // A vertex gets one node; an edge gets one chain of nodes, used by
    // both faces that meet along it. Built first, so that every face
    // mesher below finds its boundary already discretised.
    std::map<cad::EntityId, int> vertex_node;
    std::map<cad::EntityId, std::vector<int>> edge_nodes;      // in the edge's own direction
    std::map<cad::EntityId, std::vector<double>> edge_params;

    auto node_for_point = [&](const Vec3d &p, const SurfaceMesh::Provenance &where) {
        out->nodes.push_back(p);
        out->provenance.push_back(where);
        return out->NodeCount() - 1;
    };

    std::vector<cad::EntityId> faces;
    for (cad::EntityId body : bodies) {
        const cad::Body *solid = model.GetBody(body);
        if (solid == nullptr) continue;
        for (cad::EntityId shell : solid->shells) {
            for (cad::EntityId face : model.GetShell(shell)->faces) faces.push_back(face);
            for (cad::EntityId vertex : model.VerticesOfShell(shell)) {
                if (vertex_node.count(vertex) != 0) continue;
                SurfaceMesh::Provenance where;
                where.vertex = vertex;
                vertex_node[vertex] = node_for_point(model.GetVertex(vertex)->point, where);
            }
            for (cad::EntityId edge_id : model.EdgesOfShell(shell)) {
                if (edge_nodes.count(edge_id) != 0) continue;
                const cad::Edge *edge = model.GetEdge(edge_id);
                const cad::Curve3 *curve = model.CurveAt(edge->curve);
                if (curve == nullptr) continue;
                // Walk the edge, stepping by whatever the sizing field
                // asks for where we are. Stepping uniformly would ignore
                // the field entirely on an edge that runs from a fine
                // region into a coarse one.
                std::vector<double> params{edge->t_start};
                const double span = edge->t_end - edge->t_start;
                const double length = curve->Length(edge->t_start, edge->t_end, 1e-7);
                if (!(std::fabs(span) > 0.0) || !(length > 0.0)) {
                    edge_params[edge_id] = params;
                    edge_nodes[edge_id] = {};
                    continue;
                }
                double t = edge->t_start;
                for (int guard = 0; guard < 100000; ++guard) {
                    const double size = sizing.At(curve->Point(t));
                    // Parameter step for that 3D step, from the local
                    // ratio of arc length to parameter.
                    const double probe = span * 1e-4;
                    const double local = (curve->Point(t + probe) - curve->Point(t)).Length();
                    const double per_parameter = local > 0.0 ? local / std::fabs(probe)
                                                             : length / std::fabs(span);
                    double step = size / std::max(1e-30, per_parameter);
                    step = std::min(step, std::fabs(span));
                    t += span > 0.0 ? step : -step;
                    if ((span > 0.0 && t >= edge->t_end - std::fabs(span) * 1e-9) ||
                        (span < 0.0 && t <= edge->t_end + std::fabs(span) * 1e-9)) {
                        break;
                    }
                    params.push_back(t);
                }
                params.push_back(edge->t_end);
                // The two ends are the edge's vertices, which already
                // have nodes; the rest are new and belong to the edge.
                std::vector<int> chain;
                chain.push_back(vertex_node.count(edge->start_vertex) != 0
                                    ? vertex_node[edge->start_vertex]
                                    : node_for_point(curve->Point(edge->t_start), {}));
                for (std::size_t i = 1; i + 1 < params.size(); ++i) {
                    SurfaceMesh::Provenance where;
                    where.edge = edge_id;
                    where.u = params[i];
                    chain.push_back(node_for_point(curve->Point(params[i]), where));
                }
                chain.push_back(vertex_node.count(edge->end_vertex) != 0
                                    ? vertex_node[edge->end_vertex]
                                    : node_for_point(curve->Point(edge->t_end), {}));
                edge_nodes[edge_id] = chain;
                edge_params[edge_id] = params;
            }
        }
    }

    // --- One face at a time --------------------------------------------------
    for (cad::EntityId face_id : faces) {
        const cad::Face *face = model.GetFace(face_id);
        const cad::Surface *surface = model.SurfaceAt(face->surface);
        if (surface == nullptr) {
            report->warnings.push_back("face " + std::to_string(face_id) + " has no surface");
            continue;
        }

        // The boundary, in this face's parameters. Each coedge
        // contributes the shared nodes of its edge, mapped into (u, v)
        // through the p-curve so that the parameter values are this
        // face's and the *nodes* are the shared ones.
        std::vector<Vec2d> points;
        std::vector<int> global;  // parallel to points: the surface mesh node
        std::vector<std::pair<int, int>> segments;
        bool usable = true;
        for (cad::EntityId loop_id : face->loops) {
            const cad::Loop *loop = model.GetLoop(loop_id);
            if (loop == nullptr) continue;
            const int loop_start = static_cast<int>(points.size());
            for (cad::EntityId coedge_id : loop->coedges) {
                const cad::CoEdge *coedge = model.GetCoEdge(coedge_id);
                const cad::Curve3 *pcurve = model.PCurveAt(coedge->pcurve);
                const cad::Edge *edge = model.GetEdge(coedge->edge);
                if (pcurve == nullptr || edge == nullptr) {
                    usable = false;
                    break;
                }
                const std::vector<int> &chain = edge_nodes[coedge->edge];
                const std::vector<double> &params = edge_params[coedge->edge];
                if (chain.size() < 2) continue;
                double p_lo = 0.0;
                double p_hi = 0.0;
                pcurve->Domain(&p_lo, &p_hi);
                const double t_lo = edge->t_start;
                const double t_hi = edge->t_end;
                const bool forward = coedge->orientation == cad::Orientation::Forward;
                const std::size_t n = chain.size();
                for (std::size_t i = 0; i + 1 < n; ++i) {
                    const std::size_t at = forward ? i : n - 1 - i;
                    // The p-curve is parameterised over its own domain,
                    // running the way the coedge does; the edge's node i
                    // sits at the matching fraction along it.
                    const double fraction =
                        std::fabs(t_hi - t_lo) > 0.0
                            ? (params[at] - t_lo) / (t_hi - t_lo)
                            : static_cast<double>(at) / static_cast<double>(n - 1);
                    const double along = forward ? fraction : 1.0 - fraction;
                    const Vec3d on_pcurve = pcurve->Point(p_lo + (p_hi - p_lo) * along);
                    points.push_back(Vec2d{on_pcurve.x, on_pcurve.y});
                    global.push_back(chain[at]);
                }
            }
            if (!usable) break;
            const int loop_end = static_cast<int>(points.size());
            if (loop_end - loop_start < 3) continue;
            for (int i = loop_start; i < loop_end; ++i) {
                segments.push_back({i, i + 1 < loop_end ? i + 1 : loop_start});
            }
        }
        if (!usable || segments.size() < 3) {
            report->warnings.push_back("face " + std::to_string(face_id) +
                                       " could not be bounded in parameter space");
            continue;
        }

        // --- Refinement ----------------------------------------------------
        //
        // IN PASSES, NOT ONE POINT AT A TIME. Ruppert's algorithm reads
        // "find the worst triangle, split it, repeat", and implemented
        // literally that re-triangulates the whole face for every point
        // inserted: quadratic work per point, cubic overall, and it does
        // not finish. Each pass here collects every triangle that wants
        // splitting, inserts the circumcentres that are far enough apart
        // to be worth inserting, and re-triangulates once. Element size
        // roughly halves per pass where it is too big, so a handful of
        // passes does what thousands of single insertions would.
        //
        // BOUNDARY SEGMENTS ARE NEVER SPLIT, and that is a departure from
        // Ruppert made on purpose. His rule splits an encroached segment,
        // which is what guarantees both the angle bound and the
        // boundary's presence in the triangulation. But a segment here
        // lies on a CAD edge shared with the face on the other side, and
        // that face is meshed separately: splitting it here and not there
        // leaves the two faces with different nodes along their common
        // edge, and the surface no longer closes. A mesh with a crack in
        // it is worth far less than a slightly worse-shaped element
        // beside the boundary. So the edges are discretised to the sizing
        // field before any face is meshed, and a refinement point that
        // would encroach one is dropped.
        // --- Seeding the interior -------------------------------------------
        //
        // A grid in parameter space, spaced so that its *3D* spacing is
        // the size the field asks for, kept where it falls inside the
        // face. Refinement alone would get here eventually on a simple
        // face and does not get here at all on a hard one: a sphere is
        // one face whose whole boundary is its seam, used twice, so the
        // starting triangulation is two parallel chains of points with
        // nothing between them, every triangle spans the entire face,
        // and the circumcentres of such triangles land outside it. Laying
        // down a grid first turns every face into the easy case and
        // leaves refinement to do what it is good at, which is fixing up
        // what the grid could not know about.
        {
            cad::Interval extent_u;
            cad::Interval extent_v;
            for (const Vec2d &p : points) {
                extent_u.Expand(p.x);
                extent_v.Expand(p.y);
            }
            const Vec2d middle{extent_u.Mid(), extent_v.Mid()};
            const Metric metric = MetricAt(*surface, middle);
            const double size = sizing.At(surface->Point(middle.x, middle.y));
            const double step_u = size / std::sqrt(std::max(1e-30, metric.e));
            const double step_v = size / std::sqrt(std::max(1e-30, metric.g));
            const int steps_u =
                std::max(1, std::min(400, static_cast<int>(std::ceil(extent_u.Width() / step_u))));
            const int steps_v =
                std::max(1, std::min(400, static_cast<int>(std::ceil(extent_v.Width() / step_v))));
            for (int i = 1; i < steps_u; ++i) {
                for (int j = 1; j < steps_v; ++j) {
                    if (static_cast<int>(points.size()) >= options.max_points_per_face) break;
                    Vec2d at{extent_u.lo + extent_u.Width() * static_cast<double>(i) /
                                               static_cast<double>(steps_u),
                             extent_v.lo + extent_v.Width() * static_cast<double>(j) /
                                               static_cast<double>(steps_v)};
                    if (!InsidePolygon(points, segments, at)) continue;
                    // Not on top of the boundary, which would make a
                    // sliver rather than an element.
                    bool crowded = false;
                    for (std::size_t k = 0; k < points.size() && !crowded; ++k) {
                        if (MetricLength(*surface, points[k], at) < size * 0.4) crowded = true;
                    }
                    if (crowded) continue;
                    points.push_back(at);
                    SurfaceMesh::Provenance where;
                    where.face = face_id;
                    where.u = at.x;
                    where.v = at.y;
                    global.push_back(node_for_point(surface->Point(at.x, at.y), where));
                }
            }
        }

        const double min_angle = std::min(options.min_angle, 25.0) * cad::kPi / 180.0;
        std::vector<Tri> tris;
        for (int pass = 0; pass < 24; ++pass) {
            Triangulate(points, &tris);
            std::vector<Tri> inside;
            for (const Tri &t : tris) {
                const Vec2d centroid{(points[static_cast<std::size_t>(t.a)].x +
                                      points[static_cast<std::size_t>(t.b)].x +
                                      points[static_cast<std::size_t>(t.c)].x) / 3.0,
                                     (points[static_cast<std::size_t>(t.a)].y +
                                      points[static_cast<std::size_t>(t.b)].y +
                                      points[static_cast<std::size_t>(t.c)].y) / 3.0};
                if (InsidePolygon(points, segments, centroid)) inside.push_back(t);
            }
            tris = inside;
            if (static_cast<int>(points.size()) >= options.max_points_per_face) {
                report->warnings.push_back("face " + std::to_string(face_id) +
                                           " hit its refinement budget");
                break;
            }

            std::vector<Vec2d> wanted;
            for (const Tri &t : tris) {
                const Vec2d &a = points[static_cast<std::size_t>(t.a)];
                const Vec2d &b = points[static_cast<std::size_t>(t.b)];
                const Vec2d &c = points[static_cast<std::size_t>(t.c)];
                Vec2d centre;
                if (!Circumcentre(a, b, c, &centre)) continue;
                const double wanted_size = sizing.At(surface->Point(centre.x, centre.y));
                const double longest = std::max({MetricLength(*surface, a, b),
                                                 MetricLength(*surface, b, c),
                                                 MetricLength(*surface, c, a)});
                const bool too_big = longest > wanted_size * 1.3;
                const bool too_thin = SmallestAngle(*surface, a, b, c) < min_angle;
                if (!too_big && !too_thin) continue;
                if (!InsidePolygon(points, segments, centre)) continue;
                bool encroached = false;
                for (const auto &segment : segments) {
                    if (Encroaches(points[static_cast<std::size_t>(segment.first)],
                                   points[static_cast<std::size_t>(segment.second)], centre)) {
                        encroached = true;
                        break;
                    }
                }
                // ENCROACHMENT IS SKIPPED WHERE RUPPERT WOULD SPLIT, AND
                // THAT IS WHY BOUNDARY SLIVERS SURVIVE. The algorithm's
                // answer to a circumcentre that encroaches a boundary
                // segment is to split the segment instead, which is what
                // makes it terminate with a guaranteed minimum angle. It
                // cannot be done here: these segments are shared with the
                // neighbouring face, and a face that splits one on its own
                // disagrees with its neighbour about the nodes along their
                // common edge -- which was tried and broke closure. Doing
                // it properly means refining the *edge* discretisation,
                // before any face is meshed, so that both faces see the
                // same split. Until then a thin triangle against the
                // boundary is left alone, and that is the mechanism behind
                // the scale-dependence the plan records: which near-
                // degenerate triangulation comes out decides whether there
                // is such a triangle, and nothing downstream can remove it.
                if (encroached) continue;
                wanted.push_back(centre);
            }
            if (wanted.empty()) break;

            // Points too close together make slivers rather than
            // progress, so a candidate is dropped if anything is already
            // within half the size asked for there.
            //
            // THROUGH A GRID, because the obvious loop over every point
            // is quadratic per candidate and cubic per pass -- which is
            // what turned a cylinder into a hang. The grid is over
            // parameter space and the query box is the metric distance
            // converted back into parameters, so what is compared is
            // still a distance on the surface.
            cad::Interval extent_u;
            cad::Interval extent_v;
            for (const Vec2d &p : points) {
                extent_u.Expand(p.x);
                extent_v.Expand(p.y);
            }
            const int buckets = 64;
            const double bucket_u = std::max(1e-12, extent_u.Width() / buckets);
            const double bucket_v = std::max(1e-12, extent_v.Width() / buckets);
            std::map<std::pair<int, int>, std::vector<int>> grid;
            auto bucket_of = [&](const Vec2d &p) {
                return std::make_pair(
                    std::max(0, std::min(buckets - 1,
                                         static_cast<int>((p.x - extent_u.lo) / bucket_u))),
                    std::max(0, std::min(buckets - 1,
                                         static_cast<int>((p.y - extent_v.lo) / bucket_v))));
            };
            for (std::size_t i = 0; i < points.size(); ++i) {
                grid[bucket_of(points[i])].push_back(static_cast<int>(i));
            }
            int inserted = 0;
            for (const Vec2d &candidate : wanted) {
                const Vec3d at = surface->Point(candidate.x, candidate.y);
                const double keep_apart = sizing.At(at) * 0.5;
                const Metric metric = MetricAt(*surface, candidate);
                const double reach_u = keep_apart / std::sqrt(std::max(1e-30, metric.e));
                const double reach_v = keep_apart / std::sqrt(std::max(1e-30, metric.g));
                const auto lo = bucket_of(Vec2d{candidate.x - reach_u, candidate.y - reach_v});
                const auto hi = bucket_of(Vec2d{candidate.x + reach_u, candidate.y + reach_v});
                bool crowded = false;
                for (int bu = lo.first; bu <= hi.first && !crowded; ++bu) {
                    for (int bv = lo.second; bv <= hi.second && !crowded; ++bv) {
                        const auto found = grid.find({bu, bv});
                        if (found == grid.end()) continue;
                        for (int index : found->second) {
                            if (MetricLength(*surface, points[static_cast<std::size_t>(index)],
                                             candidate) < keep_apart) {
                                crowded = true;
                                break;
                            }
                        }
                    }
                }
                if (crowded) continue;
                points.push_back(candidate);
                grid[bucket_of(candidate)].push_back(static_cast<int>(points.size()) - 1);
                SurfaceMesh::Provenance where;
                where.face = face_id;
                where.u = candidate.x;
                where.v = candidate.y;
                global.push_back(node_for_point(at, where));
                ++inserted;
                if (static_cast<int>(points.size()) >= options.max_points_per_face) break;
            }
            if (inserted == 0) break;
        }


        // Emit, with the face's orientation applied so that every
        // triangle of the finished surface points out of the material.
        const bool flip = face->orientation == cad::Orientation::Reversed;
        for (const Tri &t : tris) {
            SurfaceTriangle triangle;
            triangle.face = face_id;
            triangle.a = global[static_cast<std::size_t>(t.a)];
            triangle.b = global[static_cast<std::size_t>(flip ? t.c : t.b)];
            triangle.c = global[static_cast<std::size_t>(flip ? t.b : t.c)];
            if (triangle.a == triangle.b || triangle.b == triangle.c || triangle.c == triangle.a) {
                continue;
            }
            out->triangles.push_back(triangle);
        }
    }

    // --- Welding -------------------------------------------------------------
    //
    // TWO NODES AT THE SAME PLACE ARE ONE NODE, and until they are, every
    // consumer downstream is working with geometry that is not what it
    // looks like. A vertex node and the first sample of an edge, or two
    // faces' discretisations of a shared edge, can land a few parts in a
    // billion apart -- close enough that nothing here notices and far
    // enough that they are distinct indices. The triangle between them is
    // not degenerate by area, so the check above lets it through, and the
    // tetrahedron built on it has a dihedral angle of zero.
    //
    // That is exactly what stopped the adaptive loop on an L-bracket: a
    // surface mesh of 442 nodes with one coincident pair and a shortest
    // edge of 4.2e-08 on a part ten units across, which the volume
    // mesher then reported as a quality failure. No amount of flipping or
    // smoothing repairs a degenerate input, and the error message blamed
    // the wrong stage.
    //
    // The tolerance is relative to the smallest size the field asks for
    // anywhere, because that is the shortest edge the mesher intends to
    // make and anything orders below it is not an edge, it is a
    // coincidence.
    {
        const double weld = std::max(sizing.MinSize() * 1e-6, 1e-12);
        const double cell = weld;
        std::map<std::array<long long, 3>, int> grid;
        std::vector<int> replacement(out->nodes.size(), -1);
        for (std::size_t i = 0; i < out->nodes.size(); ++i) {
            const cad::Vec3d &p = out->nodes[i];
            // Every one of the twenty-seven cells around it, so a pair
            // straddling a cell boundary is still found.
            int found = -1;
            for (int dx = -1; dx <= 1 && found < 0; ++dx) {
                for (int dy = -1; dy <= 1 && found < 0; ++dy) {
                    for (int dz = -1; dz <= 1 && found < 0; ++dz) {
                        const std::array<long long, 3> key{
                            std::llround(p.x / cell) + dx, std::llround(p.y / cell) + dy,
                            std::llround(p.z / cell) + dz};
                        const auto at = grid.find(key);
                        if (at == grid.end()) continue;
                        if ((out->nodes[static_cast<std::size_t>(at->second)] - p).Length() <= weld) {
                            found = at->second;
                        }
                    }
                }
            }
            if (found >= 0) {
                replacement[i] = found;
                continue;
            }
            replacement[i] = static_cast<int>(i);
            grid[{std::llround(p.x / cell), std::llround(p.y / cell), std::llround(p.z / cell)}] =
                static_cast<int>(i);
        }
        int welded = 0;
        for (std::size_t i = 0; i < replacement.size(); ++i) {
            if (replacement[i] != static_cast<int>(i)) ++welded;
        }
        if (welded > 0) {
            // Compact, so the volume mesher never sees a node no triangle
            // uses -- a stray point inside the surface would be inserted
            // into the triangulation and is not wanted there.
            std::vector<int> renumbered(out->nodes.size(), -1);
            std::vector<cad::Vec3d> nodes;
            std::vector<SurfaceMesh::Provenance> provenance;
            for (std::size_t i = 0; i < out->nodes.size(); ++i) {
                if (replacement[i] != static_cast<int>(i)) continue;
                renumbered[i] = static_cast<int>(nodes.size());
                nodes.push_back(out->nodes[i]);
                if (i < out->provenance.size()) provenance.push_back(out->provenance[i]);
            }
            std::vector<SurfaceTriangle> kept;
            for (SurfaceTriangle t : out->triangles) {
                t.a = renumbered[static_cast<std::size_t>(replacement[static_cast<std::size_t>(t.a)])];
                t.b = renumbered[static_cast<std::size_t>(replacement[static_cast<std::size_t>(t.b)])];
                t.c = renumbered[static_cast<std::size_t>(replacement[static_cast<std::size_t>(t.c)])];
                // A triangle whose corners welded together had no area to
                // begin with; dropping it leaves the surface closed
                // because it was never separating anything.
                if (t.a == t.b || t.b == t.c || t.c == t.a) continue;
                kept.push_back(t);
            }
            out->nodes = std::move(nodes);
            out->provenance = std::move(provenance);
            out->triangles = std::move(kept);
            report->welded_nodes = welded;
            report->warnings.push_back(std::to_string(welded) +
                                       " coincident surface nodes were welded together");
        }
    }

    report->nodes = out->NodeCount();
    report->triangles = out->TriangleCount();
    if (out->triangles.empty()) {
        report->error = "no face of these bodies could be meshed";
        return false;
    }
    report->ok = true;
    return true;
}

}  // namespace fem
