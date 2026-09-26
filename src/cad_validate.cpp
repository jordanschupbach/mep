#include "cad_validate.h"

#include "cad_pcurve.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace cad {
namespace {

void Report(ValidationReport *report, Severity severity, const char *category, const std::string &message,
            EntityId entity) {
    Diagnostic diagnostic;
    diagnostic.severity = severity;
    diagnostic.category = category;
    diagnostic.message = message;
    diagnostic.entity = entity;
    report->diagnostics.push_back(std::move(diagnostic));
}

// --- Structural ------------------------------------------------------

void CheckFaceLoops(const Model &model, const Face &face, ValidationReport *report) {
    if (face.loops.empty()) {
        Report(report, Severity::Error, "face-no-loops",
               "face " + std::to_string(face.id) + " has no loops, so it bounds nothing", face.id);
        return;
    }
    int outer = 0;
    for (EntityId l : face.loops) {
        const Loop *loop = model.GetLoop(l);
        if (loop == nullptr) {
            Report(report, Severity::Error, "dangling-reference",
                   "face " + std::to_string(face.id) + " references missing loop " + std::to_string(l), face.id);
            continue;
        }
        if (loop->is_outer) ++outer;
        if (loop->coedges.empty()) {
            Report(report, Severity::Error, "empty-loop", "loop " + std::to_string(l) + " has no coedges", l);
        }
    }
    if (outer != 1) {
        Report(report, Severity::Error, "face-outer-loops",
               "face " + std::to_string(face.id) + " has " + std::to_string(outer) +
                   " outer loops; exactly one is required",
               face.id);
    }
    if (model.SurfaceAt(face.surface) == nullptr) {
        Report(report, Severity::Error, "dangling-reference",
               "face " + std::to_string(face.id) + " has no surface", face.id);
    }
}

void CheckLoopClosure(const Model &model, const Loop &loop, const ValidationOptions &options,
                      ValidationReport *report) {
    const std::size_t count = loop.coedges.size();
    if (count == 0) return;
    for (std::size_t i = 0; i < count; ++i) {
        const EntityId current = loop.coedges[i];
        const EntityId next = loop.coedges[(i + 1) % count];
        const CoEdge *coedge = model.GetCoEdge(current);
        if (coedge == nullptr) {
            Report(report, Severity::Error, "dangling-reference",
                   "loop " + std::to_string(loop.id) + " references missing coedge " + std::to_string(current),
                   loop.id);
            return;
        }
        if (model.GetEdge(coedge->edge) == nullptr) {
            Report(report, Severity::Error, "dangling-reference",
                   "coedge " + std::to_string(current) + " references missing edge " +
                       std::to_string(coedge->edge),
                   current);
            return;
        }
        // Topological closure: the vertex chain must join up.
        const EntityId end_vertex = model.CoEdgeEndVertex(current);
        const EntityId next_start = model.CoEdgeStartVertex(next);
        if (end_vertex != next_start) {
            Report(report, Severity::Error, "loop-not-closed",
                   "loop " + std::to_string(loop.id) + " breaks at coedge " + std::to_string(current) +
                       ": it ends at vertex " + std::to_string(end_vertex) + " but the next begins at vertex " +
                       std::to_string(next_start),
                   loop.id);
        }
        // Geometric closure: and the points must actually meet.
        if (options.check_geometry) {
            const Vec3d end_point = model.CoEdgeEndPoint(current);
            const Vec3d next_point = model.CoEdgeStartPoint(next);
            if (!options.tolerance.SamePoint(end_point, next_point)) {
                Report(report, Severity::Error, "loop-gap",
                       "loop " + std::to_string(loop.id) + " has a gap of " +
                           std::to_string((end_point - next_point).Length()) + " after coedge " +
                           std::to_string(current),
                       loop.id);
            }
        }
    }
}

// --- Combinatorial ---------------------------------------------------

// The direction a coedge actually traverses its edge, as the solid sees
// it. A coedge's own orientation is relative to its face, and a face's
// orientation says whether the surface normal points out of the solid or
// into it -- so a Reversed face traverses its loops backwards. Booleans
// produce Reversed faces routinely (every face of B that ends up bounding
// the cavity in A - B is one), and comparing the raw coedge orientations
// there reports a disagreement that is not real.
Orientation EffectiveSense(const Model &model, const CoEdge &coedge) {
    const Loop *loop = model.GetLoop(coedge.loop);
    const Face *face = loop != nullptr ? model.GetFace(loop->face) : nullptr;
    const bool face_reversed = face != nullptr && face->orientation == Orientation::Reversed;
    const bool coedge_reversed = coedge.orientation == Orientation::Reversed;
    return (face_reversed != coedge_reversed) ? Orientation::Reversed : Orientation::Forward;
}

void CheckEdgeUse(const Model &model, EntityId shell_id, const ValidationOptions &options,
                  ValidationReport *report) {
    const Shell *shell = model.GetShell(shell_id);
    if (shell == nullptr) return;
    // Count coedge uses per edge, restricted to this shell's faces.
    std::map<EntityId, std::vector<EntityId>> uses;
    for (EntityId f : shell->faces) {
        const Face *face = model.GetFace(f);
        if (face == nullptr) continue;
        for (EntityId l : face->loops) {
            const Loop *loop = model.GetLoop(l);
            if (loop == nullptr) continue;
            for (EntityId c : loop->coedges) {
                const CoEdge *coedge = model.GetCoEdge(c);
                if (coedge != nullptr) uses[coedge->edge].push_back(c);
            }
        }
    }
    for (const auto &entry : uses) {
        const std::vector<EntityId> &coedges = entry.second;
        if (coedges.size() == 2) {
            // The manifold case. The two uses must run in opposite
            // senses -- that is what makes the faces agree about which
            // side the material is on. A seam edge has both uses on the
            // same face and must still satisfy it.
            const CoEdge *a = model.GetCoEdge(coedges[0]);
            const CoEdge *b = model.GetCoEdge(coedges[1]);
            if (a != nullptr && b != nullptr && EffectiveSense(model, *a) == EffectiveSense(model, *b)) {
                Report(report, Severity::Error, "edge-orientation",
                       "edge " + std::to_string(entry.first) +
                           " is used twice in the same direction; the two faces disagree about which side the "
                           "material is on",
                       entry.first);
            }
            continue;
        }
        if (coedges.size() < 2) {
            const Severity severity = options.require_closed_shells ? Severity::Error : Severity::Warning;
            Report(report, severity, "edge-free",
                   "edge " + std::to_string(entry.first) + " is used by only " +
                       std::to_string(coedges.size()) + " coedge(s); the shell is not closed there",
                   entry.first);
            continue;
        }
        Report(report, Severity::Error, "edge-non-manifold",
               "edge " + std::to_string(entry.first) + " is used by " + std::to_string(coedges.size()) +
                   " coedges; more than two faces meet along it",
               entry.first);
    }
}

// The Euler-Poincare relation for a shell:
//
//     V - E + F - H = 2 (S - G)
//
// with H the number of inner loops and G the genus. Solving for G gives a
// value that must be a non-negative integer; anything else means the
// counts are inconsistent, which is a defect no local check would find --
// a shell can have every loop closed and every edge used twice and still
// not describe a surface.
void CheckEulerPoincare(const Model &model, EntityId body_id, ValidationReport *report) {
    const Body *body = model.GetBody(body_id);
    if (body == nullptr) return;
    const TopologyCounts counts = model.CountsOfBody(body_id);
    const int characteristic = counts.vertices - counts.edges + counts.faces - counts.holes;
    // 2(S - G) = characteristic, so G = S - characteristic/2.
    if (characteristic % 2 != 0) {
        Report(report, Severity::Error, "euler-poincare",
               "body " + std::to_string(body_id) + " has V - E + F - H = " + std::to_string(characteristic) +
                   ", which is odd; no closed surface has an odd Euler characteristic, so the counts (V=" +
                   std::to_string(counts.vertices) + ", E=" + std::to_string(counts.edges) +
                   ", F=" + std::to_string(counts.faces) + ", H=" + std::to_string(counts.holes) +
                   ") are inconsistent",
               body_id);
        report->body_genus.push_back(-1);
        return;
    }
    const int genus = counts.shells - characteristic / 2;
    if (genus < 0) {
        Report(report, Severity::Error, "euler-poincare",
               "body " + std::to_string(body_id) + " implies a negative genus (" + std::to_string(genus) +
                   "), which is impossible",
               body_id);
    }
    report->body_genus.push_back(genus);
}

// --- Geometric -------------------------------------------------------

void CheckVertexOnEdges(const Model &model, const Edge &edge, const ValidationOptions &options,
                        ValidationReport *report) {
    const Curve3 *curve = model.CurveAt(edge.curve);
    if (curve == nullptr) {
        Report(report, Severity::Error, "dangling-reference",
               "edge " + std::to_string(edge.id) + " has no curve", edge.id);
        return;
    }
    const Vertex *start = model.GetVertex(edge.start_vertex);
    const Vertex *end = model.GetVertex(edge.end_vertex);
    if (start == nullptr || end == nullptr) {
        Report(report, Severity::Error, "dangling-reference",
               "edge " + std::to_string(edge.id) + " references a missing vertex", edge.id);
        return;
    }
    const Vec3d curve_start = curve->Point(edge.t_start);
    const Vec3d curve_end = curve->Point(edge.t_end);
    if (!options.tolerance.SamePoint(curve_start, start->point)) {
        Report(report, Severity::Error, "vertex-off-edge",
               "edge " + std::to_string(edge.id) + " starts " +
                   std::to_string((curve_start - start->point).Length()) + " from its start vertex",
               edge.id);
    }
    if (!options.tolerance.SamePoint(curve_end, end->point)) {
        Report(report, Severity::Error, "vertex-off-edge",
               "edge " + std::to_string(edge.id) + " ends " + std::to_string((curve_end - end->point).Length()) +
                   " from its end vertex",
               edge.id);
    }
}

void CheckEdgeOnFaces(const Model &model, const Edge &edge, const ValidationOptions &options,
                      ValidationReport *report) {
    const Curve3 *curve = model.CurveAt(edge.curve);
    if (curve == nullptr) return;
    const std::vector<EntityId> faces = model.FacesOfEdge(edge.id);
    const int samples = std::max(2, options.samples_per_edge);
    for (EntityId f : faces) {
        const Face *face = model.GetFace(f);
        if (face == nullptr) continue;
        const Surface *surface = model.SurfaceAt(face->surface);
        if (surface == nullptr) continue;
        double worst = 0.0;
        for (int i = 0; i < samples; ++i) {
            const double fraction = static_cast<double>(i) / static_cast<double>(samples - 1);
            const Vec3d point = curve->Point(edge.t_start + fraction * (edge.t_end - edge.t_start));
            double u = 0.0;
            double v = 0.0;
            Vec3d on_surface;
            if (!surface->ClosestPoint(point, &u, &v, &on_surface, options.tolerance)) continue;
            worst = std::max(worst, (point - on_surface).Length());
        }
        if (worst > options.tolerance.linear) {
            Report(report, Severity::Error, "edge-off-face",
                   "edge " + std::to_string(edge.id) + " strays " + std::to_string(worst) + " from face " +
                       std::to_string(f) + ", which claims to be bounded by it",
                   edge.id);
        }
    }
}

void CheckPCurve(const Model &model, const CoEdge &coedge, const ValidationOptions &options,
                 ValidationReport *report) {
    const Curve3 *pcurve = model.PCurveAt(coedge.pcurve);
    if (pcurve == nullptr) {
        Report(report, Severity::Warning, "pcurve-missing",
               "coedge " + std::to_string(coedge.id) + " has no p-curve, so its face cannot be trimmed",
               coedge.id);
        return;
    }
    const Loop *loop = model.GetLoop(coedge.loop);
    if (loop == nullptr) return;
    const Face *face = model.GetFace(loop->face);
    if (face == nullptr) return;
    const Surface *surface = model.SurfaceAt(face->surface);
    const Edge *edge = model.GetEdge(coedge.edge);
    if (surface == nullptr || edge == nullptr) return;
    const Curve3 *curve = model.CurveAt(edge->curve);
    if (curve == nullptr) return;

    // The p-curve, mapped through the surface, must land on the edge. The
    // check runs this way round (p-curve to 3D, not the reverse) because
    // it is the direction that catches a p-curve wandering off the edge,
    // which is what a bad seam choice produces.
    //
    // Parameterizations are deliberately not paired: the p-curve is fitted
    // and carries its own, and comparing point sets is both correct and
    // the only thing that would survive the fit being replaced.
    //
    // THE SEARCH IS OVER THE EDGE, NOT OVER ITS CURVE. Those are not the
    // same interval, and on a periodic curve they need not overlap at
    // all: an arc of a circle whose domain is [0, 2pi] may perfectly
    // legally run from 3pi/2 to 5pi/2, because a circle's parameter only
    // means anything modulo a turn. Curve3::ClosestPoint clamps to the
    // curve's declared domain rather than reporting that it ran off the
    // end, so asking it where the nearest point is answers a question
    // about the wrong interval and reports a perfectly good face as
    // having a p-curve half a radius away from its edge. This is the
    // fourth place in the kernel where that clamping has produced a wrong
    // answer rather than a refusal -- see the note in Part E.3 -- and the
    // rule by now is plain: it is the wrong tool for asking whether a
    // point is on something.
    double lo = 0.0;
    double hi = 0.0;
    pcurve->Domain(&lo, &hi);
    const int samples = std::max(2, options.samples_per_edge);
    // A sweep over the edge's own range, then a few bisection steps
    // around the best sample. Enough to measure a distance that is
    // supposed to be zero, which is all this is for.
    const int edge_samples = std::max(32, options.samples_per_edge * 2);
    auto closest_on_edge = [&](const Vec3d &point) {
        double best_t = edge->t_start;
        double best = (curve->Point(edge->t_start) - point).LengthSquared();
        for (int i = 1; i <= edge_samples; ++i) {
            const double t = edge->t_start + (edge->t_end - edge->t_start) *
                                                 static_cast<double>(i) /
                                                 static_cast<double>(edge_samples);
            const double distance = (curve->Point(t) - point).LengthSquared();
            if (distance < best) {
                best = distance;
                best_t = t;
            }
        }
        double step = (edge->t_end - edge->t_start) / static_cast<double>(edge_samples);
        for (int refine = 0; refine < 40 && std::fabs(step) > 0.0; ++refine) {
            step *= 0.5;
            for (double side : {-1.0, 1.0}) {
                const double t = best_t + side * step;
                const double distance = (curve->Point(t) - point).LengthSquared();
                if (distance < best) {
                    best = distance;
                    best_t = t;
                }
            }
        }
        return curve->Point(best_t);
    };
    double worst = 0.0;
    for (int i = 0; i < samples; ++i) {
        const double t = lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(samples - 1);
        const Vec2d uv = PCurvePoint(*pcurve, t);
        const Vec3d from_surface = surface->Point(uv.x, uv.y);
        worst = std::max(worst, (from_surface - closest_on_edge(from_surface)).Length());
    }
    if (worst > options.tolerance.linear) {
        Report(report, Severity::Error, "pcurve-off-edge",
               "coedge " + std::to_string(coedge.id) + "'s p-curve maps to points up to " +
                   std::to_string(worst) + " from the edge it represents",
               coedge.id);
    }
    // And its ends must be the coedge's ends, in the coedge's direction.
    const Vec2d start_uv = PCurvePoint(*pcurve, lo);
    const Vec2d end_uv = PCurvePoint(*pcurve, hi);
    if (!options.tolerance.SamePoint(surface->Point(start_uv.x, start_uv.y), model.CoEdgeStartPoint(coedge.id))) {
        Report(report, Severity::Error, "pcurve-endpoint",
               "coedge " + std::to_string(coedge.id) + "'s p-curve does not start where the coedge does",
               coedge.id);
    }
    if (!options.tolerance.SamePoint(surface->Point(end_uv.x, end_uv.y), model.CoEdgeEndPoint(coedge.id))) {
        Report(report, Severity::Error, "pcurve-endpoint",
               "coedge " + std::to_string(coedge.id) + "'s p-curve does not end where the coedge does",
               coedge.id);
    }
}

void CheckLoopOrientation(const Model &model, const Loop &loop, ValidationReport *report) {
    // Needs p-curves; silently skipped without them, since CheckPCurve
    // has already warned.
    for (EntityId c : loop.coedges) {
        const CoEdge *coedge = model.GetCoEdge(c);
        if (coedge == nullptr || model.PCurveAt(coedge->pcurve) == nullptr) return;
    }
    const double area = LoopSignedArea(model, loop.id);
    if (loop.is_outer && area <= 0.0) {
        Report(report, Severity::Error, "loop-orientation",
               "outer loop " + std::to_string(loop.id) + " encloses signed area " + std::to_string(area) +
                   " in parameter space; an outer loop must run counter-clockwise (positive)",
               loop.id);
    }
    if (!loop.is_outer && area >= 0.0) {
        Report(report, Severity::Error, "loop-orientation",
               "inner loop " + std::to_string(loop.id) + " encloses signed area " + std::to_string(area) +
                   " in parameter space; a hole must run clockwise (negative)",
               loop.id);
    }
}

}  // namespace

bool ValidationReport::Ok() const { return ErrorCount() == 0; }

int ValidationReport::ErrorCount() const {
    int count = 0;
    for (const Diagnostic &d : diagnostics) {
        if (d.severity == Severity::Error) ++count;
    }
    return count;
}

int ValidationReport::WarningCount() const {
    return static_cast<int>(diagnostics.size()) - ErrorCount();
}

std::vector<const Diagnostic *> ValidationReport::OfCategory(const std::string &category) const {
    std::vector<const Diagnostic *> result;
    for (const Diagnostic &d : diagnostics) {
        if (d.category == category) result.push_back(&d);
    }
    return result;
}

std::string ValidationReport::Summary() const {
    if (diagnostics.empty()) return "valid";
    std::string text = std::to_string(ErrorCount()) + " error(s), " + std::to_string(WarningCount()) +
                       " warning(s):";
    for (const Diagnostic &d : diagnostics) {
        text += "\n  [";
        text += (d.severity == Severity::Error ? "error" : "warning");
        text += "] " + d.category + ": " + d.message;
    }
    return text;
}

bool Validate(const Model &model, ValidationReport *report, const ValidationOptions &options) {
    report->diagnostics.clear();
    report->body_genus.clear();

    for (const Face &face : model.Faces()) CheckFaceLoops(model, face, report);
    for (const Loop &loop : model.Loops()) CheckLoopClosure(model, loop, options, report);
    for (const Shell &shell : model.Shells()) CheckEdgeUse(model, shell.id, options, report);
    for (const Body &body : model.Bodies()) CheckEulerPoincare(model, body.id, report);

    if (options.check_geometry) {
        for (const Edge &edge : model.Edges()) {
            CheckVertexOnEdges(model, edge, options, report);
            CheckEdgeOnFaces(model, edge, options, report);
        }
        for (const CoEdge &coedge : model.CoEdges()) CheckPCurve(model, coedge, options, report);
        for (const Loop &loop : model.Loops()) CheckLoopOrientation(model, loop, report);
    }
    return report->Ok();
}

bool ValidateBody(const Model &model, EntityId body_id, ValidationReport *report,
                  const ValidationOptions &options) {
    report->diagnostics.clear();
    report->body_genus.clear();
    const Body *body = model.GetBody(body_id);
    if (body == nullptr) {
        Report(report, Severity::Error, "dangling-reference", "no such body", body_id);
        return false;
    }
    for (EntityId s : body->shells) {
        const Shell *shell = model.GetShell(s);
        if (shell == nullptr) continue;
        CheckEdgeUse(model, s, options, report);
        for (EntityId f : shell->faces) {
            const Face *face = model.GetFace(f);
            if (face == nullptr) continue;
            CheckFaceLoops(model, *face, report);
            for (EntityId l : face->loops) {
                const Loop *loop = model.GetLoop(l);
                if (loop == nullptr) continue;
                CheckLoopClosure(model, *loop, options, report);
                if (options.check_geometry) {
                    CheckLoopOrientation(model, *loop, report);
                    for (EntityId c : loop->coedges) {
                        const CoEdge *coedge = model.GetCoEdge(c);
                        if (coedge != nullptr) CheckPCurve(model, *coedge, options, report);
                    }
                }
            }
        }
        if (options.check_geometry) {
            for (EntityId e : model.EdgesOfShell(s)) {
                const Edge *edge = model.GetEdge(e);
                if (edge == nullptr) continue;
                CheckVertexOnEdges(model, *edge, options, report);
                CheckEdgeOnFaces(model, *edge, options, report);
            }
        }
    }
    CheckEulerPoincare(model, body_id, report);
    return report->Ok();
}

}  // namespace cad
