#include "cad_topology.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace cad {
namespace {

inline std::size_t Idx(EntityId id) { return static_cast<std::size_t>(id); }

template <typename T>
const T *Lookup(const std::vector<T> &items, EntityId id) {
    if (id < 0 || Idx(id) >= items.size()) return nullptr;
    return &items[Idx(id)];
}

}  // namespace

// ---------------------------------------------------------------------
// Geometry
// ---------------------------------------------------------------------

int Model::AddCurve(std::shared_ptr<const Curve3> curve) {
    curves_.push_back(std::move(curve));
    return static_cast<int>(curves_.size()) - 1;
}

int Model::AddSurface(std::shared_ptr<const Surface> surface) {
    surfaces_.push_back(std::move(surface));
    return static_cast<int>(surfaces_.size()) - 1;
}

int Model::AddPCurve(std::shared_ptr<const Curve3> pcurve) {
    pcurves_.push_back(std::move(pcurve));
    return static_cast<int>(pcurves_.size()) - 1;
}

const Curve3 *Model::CurveAt(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= curves_.size()) return nullptr;
    return curves_[static_cast<std::size_t>(index)].get();
}

const Surface *Model::SurfaceAt(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= surfaces_.size()) return nullptr;
    return surfaces_[static_cast<std::size_t>(index)].get();
}

const Curve3 *Model::PCurveAt(int index) const {
    if (index < 0 || static_cast<std::size_t>(index) >= pcurves_.size()) return nullptr;
    return pcurves_[static_cast<std::size_t>(index)].get();
}

// ---------------------------------------------------------------------
// Topology creation
// ---------------------------------------------------------------------

EntityId Model::AddVertex(const Vec3d &point, double tolerance) {
    Vertex v;
    v.id = static_cast<EntityId>(vertices_.size());
    v.point = point;
    v.tolerance = tolerance;
    vertices_.push_back(v);
    return v.id;
}

EntityId Model::AddEdge(int curve, EntityId start_vertex, EntityId end_vertex, double t_start, double t_end,
                        double tolerance) {
    Edge e;
    e.id = static_cast<EntityId>(edges_.size());
    e.curve = curve;
    e.start_vertex = start_vertex;
    e.end_vertex = end_vertex;
    e.t_start = t_start;
    e.t_end = t_end;
    e.tolerance = tolerance;
    edges_.push_back(e);
    return e.id;
}

EntityId Model::AddCoEdge(EntityId edge, Orientation orientation, int pcurve) {
    CoEdge c;
    c.id = static_cast<EntityId>(coedges_.size());
    c.edge = edge;
    c.orientation = orientation;
    c.pcurve = pcurve;
    coedges_.push_back(c);
    return c.id;
}

EntityId Model::AddLoop(const std::vector<EntityId> &coedges, bool is_outer) {
    Loop l;
    l.id = static_cast<EntityId>(loops_.size());
    l.coedges = coedges;
    l.is_outer = is_outer;
    loops_.push_back(l);
    // Back-link, so a coedge can find its loop without a search.
    for (EntityId c : coedges) {
        if (CoEdge *coedge = GetCoEdge(c)) coedge->loop = l.id;
    }
    return l.id;
}

EntityId Model::AddFace(int surface, Orientation orientation, const std::vector<EntityId> &loops,
                        const std::string &name, double tolerance) {
    Face f;
    f.id = static_cast<EntityId>(faces_.size());
    f.surface = surface;
    f.orientation = orientation;
    f.loops = loops;
    f.name = name;
    f.tolerance = tolerance;
    faces_.push_back(f);
    for (EntityId l : loops) {
        if (Loop *loop = GetLoop(l)) loop->face = f.id;
    }
    return f.id;
}

EntityId Model::AddShell(const std::vector<EntityId> &faces, bool is_outer) {
    Shell s;
    s.id = static_cast<EntityId>(shells_.size());
    s.faces = faces;
    s.is_outer = is_outer;
    shells_.push_back(s);
    for (EntityId f : faces) {
        if (Face *face = GetFace(f)) face->shell = s.id;
    }
    return s.id;
}

EntityId Model::AddBody(const std::vector<EntityId> &shells, const std::string &name) {
    Body b;
    b.id = static_cast<EntityId>(bodies_.size());
    b.shells = shells;
    b.name = name;
    bodies_.push_back(b);
    for (EntityId s : shells) {
        if (Shell *shell = GetShell(s)) shell->body = b.id;
    }
    return b.id;
}

// ---------------------------------------------------------------------
// Access
// ---------------------------------------------------------------------

const Vertex *Model::GetVertex(EntityId id) const { return Lookup(vertices_, id); }
const Edge *Model::GetEdge(EntityId id) const { return Lookup(edges_, id); }
const CoEdge *Model::GetCoEdge(EntityId id) const { return Lookup(coedges_, id); }
const Loop *Model::GetLoop(EntityId id) const { return Lookup(loops_, id); }
const Face *Model::GetFace(EntityId id) const { return Lookup(faces_, id); }
const Shell *Model::GetShell(EntityId id) const { return Lookup(shells_, id); }
const Body *Model::GetBody(EntityId id) const { return Lookup(bodies_, id); }

Vertex *Model::GetVertex(EntityId id) { return const_cast<Vertex *>(Lookup(vertices_, id)); }
Edge *Model::GetEdge(EntityId id) { return const_cast<Edge *>(Lookup(edges_, id)); }
CoEdge *Model::GetCoEdge(EntityId id) { return const_cast<CoEdge *>(Lookup(coedges_, id)); }
Loop *Model::GetLoop(EntityId id) { return const_cast<Loop *>(Lookup(loops_, id)); }
Face *Model::GetFace(EntityId id) { return const_cast<Face *>(Lookup(faces_, id)); }
Shell *Model::GetShell(EntityId id) { return const_cast<Shell *>(Lookup(shells_, id)); }
Body *Model::GetBody(EntityId id) { return const_cast<Body *>(Lookup(bodies_, id)); }

// ---------------------------------------------------------------------
// Derived queries
// ---------------------------------------------------------------------

std::vector<EntityId> Model::CoEdgesOfEdge(EntityId edge) const {
    std::vector<EntityId> result;
    for (const CoEdge &c : coedges_) {
        if (c.edge == edge) result.push_back(c.id);
    }
    return result;
}

std::vector<EntityId> Model::FacesOfEdge(EntityId edge) const {
    std::vector<EntityId> result;
    for (EntityId c : CoEdgesOfEdge(edge)) {
        const CoEdge *coedge = GetCoEdge(c);
        if (coedge == nullptr) continue;
        const Loop *loop = GetLoop(coedge->loop);
        if (loop == nullptr || loop->face == kNoEntity) continue;
        if (std::find(result.begin(), result.end(), loop->face) == result.end()) result.push_back(loop->face);
    }
    return result;
}

std::vector<EntityId> Model::EdgesOfFace(EntityId face) const {
    std::vector<EntityId> result;
    const Face *f = GetFace(face);
    if (f == nullptr) return result;
    for (EntityId l : f->loops) {
        const Loop *loop = GetLoop(l);
        if (loop == nullptr) continue;
        for (EntityId c : loop->coedges) {
            const CoEdge *coedge = GetCoEdge(c);
            if (coedge == nullptr) continue;
            if (std::find(result.begin(), result.end(), coedge->edge) == result.end()) {
                result.push_back(coedge->edge);
            }
        }
    }
    return result;
}

std::vector<EntityId> Model::EdgesOfShell(EntityId shell) const {
    std::vector<EntityId> result;
    const Shell *s = GetShell(shell);
    if (s == nullptr) return result;
    for (EntityId f : s->faces) {
        for (EntityId e : EdgesOfFace(f)) {
            if (std::find(result.begin(), result.end(), e) == result.end()) result.push_back(e);
        }
    }
    return result;
}

std::vector<EntityId> Model::VerticesOfShell(EntityId shell) const {
    std::vector<EntityId> result;
    for (EntityId e : EdgesOfShell(shell)) {
        const Edge *edge = GetEdge(e);
        if (edge == nullptr) continue;
        for (EntityId v : {edge->start_vertex, edge->end_vertex}) {
            if (v != kNoEntity && std::find(result.begin(), result.end(), v) == result.end()) {
                result.push_back(v);
            }
        }
    }
    return result;
}

TopologyCounts Model::CountsOfShell(EntityId shell) const {
    TopologyCounts counts;
    const Shell *s = GetShell(shell);
    if (s == nullptr) return counts;
    counts.shells = 1;
    counts.faces = static_cast<int>(s->faces.size());
    counts.edges = static_cast<int>(EdgesOfShell(shell).size());
    counts.vertices = static_cast<int>(VerticesOfShell(shell).size());
    for (EntityId f : s->faces) {
        const Face *face = GetFace(f);
        if (face == nullptr) continue;
        counts.loops += static_cast<int>(face->loops.size());
        for (EntityId l : face->loops) {
            const Loop *loop = GetLoop(l);
            if (loop != nullptr && !loop->is_outer) ++counts.holes;
        }
    }
    return counts;
}

TopologyCounts Model::CountsOfBody(EntityId body) const {
    TopologyCounts counts;
    const Body *b = GetBody(body);
    if (b == nullptr) return counts;
    for (EntityId s : b->shells) {
        const TopologyCounts shell_counts = CountsOfShell(s);
        counts.vertices += shell_counts.vertices;
        counts.edges += shell_counts.edges;
        counts.faces += shell_counts.faces;
        counts.loops += shell_counts.loops;
        counts.shells += shell_counts.shells;
        counts.holes += shell_counts.holes;
    }
    return counts;
}

Vec3d Model::EdgePoint(EntityId edge, double t) const {
    const Edge *e = GetEdge(edge);
    if (e == nullptr) return Vec3d{};
    const Curve3 *curve = CurveAt(e->curve);
    if (curve == nullptr) return Vec3d{};
    return curve->Point(t);
}

Vec3d Model::EdgeStartPoint(EntityId edge) const {
    const Edge *e = GetEdge(edge);
    return e != nullptr ? EdgePoint(edge, e->t_start) : Vec3d{};
}

Vec3d Model::EdgeEndPoint(EntityId edge) const {
    const Edge *e = GetEdge(edge);
    return e != nullptr ? EdgePoint(edge, e->t_end) : Vec3d{};
}

Vec3d Model::CoEdgeStartPoint(EntityId coedge) const {
    const CoEdge *c = GetCoEdge(coedge);
    if (c == nullptr) return Vec3d{};
    return c->orientation == Orientation::Forward ? EdgeStartPoint(c->edge) : EdgeEndPoint(c->edge);
}

Vec3d Model::CoEdgeEndPoint(EntityId coedge) const {
    const CoEdge *c = GetCoEdge(coedge);
    if (c == nullptr) return Vec3d{};
    return c->orientation == Orientation::Forward ? EdgeEndPoint(c->edge) : EdgeStartPoint(c->edge);
}

EntityId Model::CoEdgeStartVertex(EntityId coedge) const {
    const CoEdge *c = GetCoEdge(coedge);
    if (c == nullptr) return kNoEntity;
    const Edge *e = GetEdge(c->edge);
    if (e == nullptr) return kNoEntity;
    return c->orientation == Orientation::Forward ? e->start_vertex : e->end_vertex;
}

EntityId Model::CoEdgeEndVertex(EntityId coedge) const {
    const CoEdge *c = GetCoEdge(coedge);
    if (c == nullptr) return kNoEntity;
    const Edge *e = GetEdge(c->edge);
    if (e == nullptr) return kNoEntity;
    return c->orientation == Orientation::Forward ? e->end_vertex : e->start_vertex;
}

Vec3d Model::FaceNormal(EntityId face, double u, double v) const {
    const Face *f = GetFace(face);
    if (f == nullptr) return Vec3d{};
    const Surface *surface = SurfaceAt(f->surface);
    if (surface == nullptr) return Vec3d{};
    const Vec3d normal = surface->Normal(u, v);
    return f->orientation == Orientation::Forward ? normal : -normal;
}

Box3d Model::BoundsOfFace(EntityId face) const {
    const Face *f = GetFace(face);
    if (f == nullptr) return Box3d{};
    const Surface *surface = SurfaceAt(f->surface);
    return surface != nullptr ? surface->Bounds() : Box3d{};
}

Box3d Model::Bounds() const {
    Box3d box;
    for (const Vertex &v : vertices_) box.Expand(v.point);
    // Vertices alone miss a face that bulges past them -- a cylinder's
    // tube reaches further than its seam vertices do -- so every face's
    // own bound is folded in as well.
    for (const Face &f : faces_) {
        const Surface *surface = SurfaceAt(f.surface);
        if (surface != nullptr) box.Expand(surface->Bounds());
    }
    return box;
}

// ---------------------------------------------------------------------
// Primitives
// ---------------------------------------------------------------------

namespace {

// Find-or-create an edge between two vertices along a given curve. Box
// faces share every edge with a neighbour, and creating it twice would
// produce a non-manifold model that looks fine until the validity
// checker counts coedges per edge.
class EdgeCache {
public:
    explicit EdgeCache(Model *model) : model_(model) {}

    EntityId LineEdge(EntityId a, EntityId b) {
        const auto key = std::make_pair(std::min(a, b), std::max(a, b));
        const auto found = cache_.find(key);
        if (found != cache_.end()) return found->second;
        const Vec3d pa = model_->GetVertex(a)->point;
        const Vec3d pb = model_->GetVertex(b)->point;
        const int curve = model_->AddCurve(std::make_shared<Line3>(Line3::FromPoints(pa, pb)));
        const EntityId edge = model_->AddEdge(curve, a, b, 0.0, 1.0);
        cache_.emplace(key, edge);
        return edge;
    }

    // Was the cached edge created with `a` as its start? Tells a caller
    // which orientation its coedge needs.
    Orientation OrientationFor(EntityId edge, EntityId from) const {
        const Edge *e = model_->GetEdge(edge);
        return (e != nullptr && e->start_vertex == from) ? Orientation::Forward : Orientation::Reversed;
    }

private:
    Model *model_;
    std::map<std::pair<EntityId, EntityId>, EntityId> cache_;
};

}  // namespace

bool MakeBox(const Vec3d &min_corner, const Vec3d &size, Model *model, EntityId *out_body) {
    if (!(size.x > 0.0 && size.y > 0.0 && size.z > 0.0)) return false;
    const Vec3d max_corner = min_corner + size;

    // Corner indexing: bit 0 is x, bit 1 is y, bit 2 is z.
    EntityId corner[8];
    for (int i = 0; i < 8; ++i) {
        const Vec3d p{(i & 1) ? max_corner.x : min_corner.x, (i & 2) ? max_corner.y : min_corner.y,
                      (i & 4) ? max_corner.z : min_corner.z};
        corner[i] = model->AddVertex(p);
    }

    EdgeCache edges(model);
    std::vector<EntityId> face_ids;

    // Each face is given axes whose cross product is already the *outward*
    // normal, so every face's orientation is Forward and the model never
    // has to reason about a flipped surface. The four corners are listed
    // counter-clockwise in that face's own (u,v), which is what makes the
    // loop an outer boundary rather than a hole.
    struct FaceSpec {
        Vec3d origin;
        Vec3d x_axis;
        Vec3d y_axis;
        double width;
        double height;
        int corners[4];
        const char *name;
    };
    const FaceSpec specs[6] = {
        // x = min: outward -x, so (y cross z) reversed -> axes (z, y).
        {min_corner, Vec3d{0, 0, 1}, Vec3d{0, 1, 0}, size.z, size.y, {0, 4, 6, 2}, "x_min"},
        // x = max: outward +x -> axes (y, z).
        {Vec3d{max_corner.x, min_corner.y, min_corner.z}, Vec3d{0, 1, 0}, Vec3d{0, 0, 1}, size.y, size.z,
         {1, 3, 7, 5}, "x_max"},
        // y = min: outward -y -> axes (x, z).
        {min_corner, Vec3d{1, 0, 0}, Vec3d{0, 0, 1}, size.x, size.z, {0, 1, 5, 4}, "y_min"},
        // y = max: outward +y -> axes (z, x).
        {Vec3d{min_corner.x, max_corner.y, min_corner.z}, Vec3d{0, 0, 1}, Vec3d{1, 0, 0}, size.z, size.x,
         {2, 6, 7, 3}, "y_max"},
        // z = min: outward -z -> axes (y, x).
        {min_corner, Vec3d{0, 1, 0}, Vec3d{1, 0, 0}, size.y, size.x, {0, 2, 3, 1}, "z_min"},
        // z = max: outward +z -> axes (x, y).
        {Vec3d{min_corner.x, min_corner.y, max_corner.z}, Vec3d{1, 0, 0}, Vec3d{0, 1, 0}, size.x, size.y,
         {4, 5, 7, 6}, "z_max"},
    };

    for (const FaceSpec &spec : specs) {
        const int surface = model->AddSurface(std::make_shared<PlaneSurface>(spec.origin, spec.x_axis,
                                                                             spec.y_axis, 0.0, spec.width, 0.0,
                                                                             spec.height));
        std::vector<EntityId> coedges;
        for (int i = 0; i < 4; ++i) {
            const EntityId from = corner[spec.corners[i]];
            const EntityId to = corner[spec.corners[(i + 1) % 4]];
            const EntityId edge = edges.LineEdge(from, to);
            coedges.push_back(model->AddCoEdge(edge, edges.OrientationFor(edge, from)));
        }
        const EntityId loop = model->AddLoop(coedges, true);
        face_ids.push_back(model->AddFace(surface, Orientation::Forward, {loop}, spec.name));
    }

    const EntityId shell = model->AddShell(face_ids, true);
    *out_body = model->AddBody({shell}, "box");
    return true;
}

bool MakeCylinder(const Vec3d &base_center, const Vec3d &axis_in, double radius, double height, Model *model,
                  EntityId *out_body) {
    if (!(radius > 0.0 && height > 0.0)) return false;
    const Vec3d axis = axis_in.Normalized();
    const Vec3d x_axis = axis.AnyPerpendicular();
    const Vec3d y_axis = axis.Cross(x_axis);
    const Vec3d top_center = base_center + axis * height;

    // Two vertices, both on the seam: where it meets the bottom rim and
    // where it meets the top rim.
    const EntityId bottom_seam = model->AddVertex(base_center + x_axis * radius);
    const EntityId top_seam = model->AddVertex(top_center + x_axis * radius);

    const int bottom_circle_curve =
        model->AddCurve(std::make_shared<Circle3>(base_center, x_axis, y_axis, radius, 0.0, kTwoPi));
    const int top_circle_curve =
        model->AddCurve(std::make_shared<Circle3>(top_center, x_axis, y_axis, radius, 0.0, kTwoPi));
    const int seam_curve = model->AddCurve(std::make_shared<Line3>(
        Line3::FromPoints(base_center + x_axis * radius, top_center + x_axis * radius)));

    // Both circular edges are *closed*: they start and end at the same
    // seam vertex, having gone all the way round.
    const EntityId bottom_edge =
        model->AddEdge(bottom_circle_curve, bottom_seam, bottom_seam, 0.0, kTwoPi);
    const EntityId top_edge = model->AddEdge(top_circle_curve, top_seam, top_seam, 0.0, kTwoPi);
    const EntityId seam_edge = model->AddEdge(seam_curve, bottom_seam, top_seam, 0.0, 1.0);

    // The tube. Its parameter rectangle is [0, 2pi] x [0, height], and the
    // loop walks that rectangle counter-clockwise: along the bottom in
    // +u, up the seam at u = 2pi, back along the top in -u, and down the
    // seam again at u = 0. The two seam coedges are the same edge used
    // twice by one loop, which is exactly what a seam is.
    const int tube_surface = model->AddSurface(
        std::make_shared<CylinderSurface>(base_center, x_axis, axis, radius, 0.0, kTwoPi, 0.0, height));
    std::vector<EntityId> tube_coedges;
    tube_coedges.push_back(model->AddCoEdge(bottom_edge, Orientation::Forward));
    tube_coedges.push_back(model->AddCoEdge(seam_edge, Orientation::Forward));
    tube_coedges.push_back(model->AddCoEdge(top_edge, Orientation::Reversed));
    tube_coedges.push_back(model->AddCoEdge(seam_edge, Orientation::Reversed));
    const EntityId tube_loop = model->AddLoop(tube_coedges, true);
    const EntityId tube_face =
        model->AddFace(tube_surface, Orientation::Forward, {tube_loop}, "side");

    // The caps. Each is a plane whose parameter rectangle comfortably
    // contains the disc; the loop is the rim. The plane's axes are chosen
    // so its normal already points out of the solid.
    const int bottom_surface = model->AddSurface(std::make_shared<PlaneSurface>(
        base_center, y_axis, x_axis, -radius * 1.5, radius * 1.5, -radius * 1.5, radius * 1.5));
    const EntityId bottom_loop =
        model->AddLoop({model->AddCoEdge(bottom_edge, Orientation::Reversed)}, true);
    const EntityId bottom_face =
        model->AddFace(bottom_surface, Orientation::Forward, {bottom_loop}, "bottom");

    const int top_surface = model->AddSurface(std::make_shared<PlaneSurface>(
        top_center, x_axis, y_axis, -radius * 1.5, radius * 1.5, -radius * 1.5, radius * 1.5));
    const EntityId top_loop = model->AddLoop({model->AddCoEdge(top_edge, Orientation::Forward)}, true);
    const EntityId top_face = model->AddFace(top_surface, Orientation::Forward, {top_loop}, "top");

    const EntityId shell = model->AddShell({tube_face, bottom_face, top_face}, true);
    *out_body = model->AddBody({shell}, "cylinder");
    return true;
}

bool MakeSphere(const Vec3d &center, double radius, Model *model, EntityId *out_body) {
    if (!(radius > 0.0)) return false;
    const Vec3d axis{0.0, 0.0, 1.0};
    const Vec3d x_axis{1.0, 0.0, 0.0};

    // The two poles. These are genuine singular points of the surface --
    // the normal is undefined there and every longitude meets -- but they
    // are ordinary vertices as far as the topology is concerned.
    const EntityId south = model->AddVertex(center - axis * radius);
    const EntityId north = model->AddVertex(center + axis * radius);

    // The seam: the meridian at longitude u = 0, running south pole to
    // north pole. It has to be at u = 0 specifically, not at some
    // arbitrary longitude -- the face is the whole parameter rectangle
    // [0, 2pi] x [-pi/2, pi/2], so its boundary is at u = 0 and u = 2pi,
    // and a seam anywhere else would not bound the face at all.
    //
    // SphereSurface::Point(0, v) is centre + r*cos(v)*x_axis +
    // r*sin(v)*axis, so as a Circle3 the meridian's own frame is
    // (x_axis, axis) swept from -pi/2 to +pi/2 -- which makes its
    // parameter equal to the latitude v, a correspondence the p-curve
    // construction then gets for free.
    const int seam_curve = model->AddCurve(
        std::make_shared<Circle3>(center, x_axis, axis, radius, -kHalfPi, kHalfPi));
    const EntityId seam_edge = model->AddEdge(seam_curve, south, north, -kHalfPi, kHalfPi);

    const int surface =
        model->AddSurface(std::make_shared<SphereSurface>(center, radius, x_axis, axis, 0.0, kTwoPi,
                                                          -kHalfPi, kHalfPi));
    // The loop is the seam used twice. The two pole "sides" of the
    // parameter rectangle are degenerate -- they collapse to a point --
    // so they contribute no coedge, which is the standard way to close a
    // spherical face and is what makes V - E + F come out at 2.
    std::vector<EntityId> coedges;
    coedges.push_back(model->AddCoEdge(seam_edge, Orientation::Forward));
    coedges.push_back(model->AddCoEdge(seam_edge, Orientation::Reversed));
    const EntityId loop = model->AddLoop(coedges, true);
    const EntityId face = model->AddFace(surface, Orientation::Forward, {loop}, "sphere");
    const EntityId shell = model->AddShell({face}, true);
    *out_body = model->AddBody({shell}, "sphere");
    return true;
}

bool MakeTorus(const Vec3d &center, const Vec3d &axis_in, double major_radius, double minor_radius,
               Model *model, EntityId *out_body) {
    if (!(major_radius > minor_radius && minor_radius > 0.0)) return false;
    const Vec3d axis = axis_in.Normalized();
    const Vec3d x_axis = axis.AnyPerpendicular();
    const Vec3d y_axis = axis.Cross(x_axis);

    // One vertex, where the two seams cross.
    const Vec3d seam_point = center + x_axis * (major_radius + minor_radius);
    const EntityId vertex = model->AddVertex(seam_point);

    // Seam A: around the tube, at u = 0. Seam B: around the axis, at
    // v = 0 (the outer equator).
    const Vec3d tube_center = center + x_axis * major_radius;
    const int tube_curve =
        model->AddCurve(std::make_shared<Circle3>(tube_center, x_axis, axis, minor_radius, 0.0, kTwoPi));
    const int equator_curve = model->AddCurve(
        std::make_shared<Circle3>(center, x_axis, y_axis, major_radius + minor_radius, 0.0, kTwoPi));
    const EntityId tube_edge = model->AddEdge(tube_curve, vertex, vertex, 0.0, kTwoPi);
    const EntityId equator_edge = model->AddEdge(equator_curve, vertex, vertex, 0.0, kTwoPi);

    const int surface = model->AddSurface(std::make_shared<TorusSurface>(
        center, x_axis, axis, major_radius, minor_radius, 0.0, kTwoPi, 0.0, kTwoPi));
    // The parameter rectangle walked counter-clockwise, with both pairs
    // of sides being seams: along v = 0 in +u, around the tube at
    // u = 2pi in +v, back along v = 2pi in -u, and around the tube again
    // at u = 0 in -v.
    std::vector<EntityId> coedges;
    coedges.push_back(model->AddCoEdge(equator_edge, Orientation::Forward));
    coedges.push_back(model->AddCoEdge(tube_edge, Orientation::Forward));
    coedges.push_back(model->AddCoEdge(equator_edge, Orientation::Reversed));
    coedges.push_back(model->AddCoEdge(tube_edge, Orientation::Reversed));
    const EntityId loop = model->AddLoop(coedges, true);
    const EntityId face = model->AddFace(surface, Orientation::Forward, {loop}, "torus");
    const EntityId shell = model->AddShell({face}, true);
    *out_body = model->AddBody({shell}, "torus");
    return true;
}

}  // namespace cad
