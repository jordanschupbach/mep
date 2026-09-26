#include "cad_boolean.h"

#include "cad_arrangement.h"
#include "cad_pcurve.h"
#include "cad_validate.h"

#include <limits>
#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <tuple>

namespace cad {
namespace {


Vec2d At(const Curve3 &pcurve, double t) {
    const Vec3d p = pcurve.Point(t);
    return Vec2d{p.x, p.y};
}

// A piece of curve in one face's parameter space, together with the 3D
// curve it corresponds to. Both are carried because the arrangement
// reasons in parameter space while the edges it produces must be built
// from the exact 3D geometry.
struct Segment {
    std::shared_ptr<const Curve3> pcurve;
    double p0 = 0.0;
    double p1 = 1.0;
    std::shared_ptr<const Curve3> curve3d;
    double c0 = 0.0;
    double c1 = 1.0;
    bool from_intersection = false;
};

// The 3D-curve parameter matching a parameter-space parameter.
//
// The two curves were fitted independently and do not share a
// parameterization -- the same problem Part A's ToNurbs note describes --
// so the correspondence is made through geometry rather than by pairing
// parameters. Evaluate the p-curve, map it onto the surface, and invert
// that point on the 3D curve.
bool CorrespondingParameter(const Segment &segment, const Surface &surface, double p, double *out_c) {
    const Vec2d uv = At(*segment.pcurve, p);
    const Vec3d point = surface.Point(uv.x, uv.y);
    double t = 0.0;
    if (!segment.curve3d->ClosestPoint(point, &t, nullptr)) return false;
    *out_c = t;
    return true;
}

// Whether a surface's parameterization has a pole: a parameter-space
// boundary that collapses to a single point in space (a sphere's two
// poles, a cone's apex, any surface of revolution meeting its axis).
//
// Such a face cannot yet be cut. The arrangement works in parameter
// space, and at a pole the face's own boundary there is not a curve but a
// degenerate line that carries no p-curve segment, so the parameter
// rectangle never closes and the cycles that would bound the pieces do
// not exist. Compounding it, an intersection curve crossing the seam is
// delivered unwrapped -- a sphere cut by a plane came back spanning u
// from pi/2 to pi/2 + 2*pi, half of it outside the face's own domain.
//
// Both are fixable, and neither is a small fix, so the case is detected
// and refused here rather than discovered as an unexplained invalid
// result several steps later.
bool HasParameterPole(const Surface &surface) {
    double u0 = 0.0;
    double u1 = 0.0;
    double v0 = 0.0;
    double v1 = 0.0;
    surface.Domain(&u0, &u1, &v0, &v1);
    const double scale = std::max(1e-12, surface.Point(u0, v0).Length() + (u1 - u0) + (v1 - v0));
    constexpr int kSamples = 5;
    for (int edge = 0; edge < 4; ++edge) {
        for (int i = 0; i <= kSamples; ++i) {
            const double f = static_cast<double>(i) / static_cast<double>(kSamples);
            const double u = (edge < 2) ? u0 + f * (u1 - u0) : (edge == 2 ? u0 : u1);
            const double v = (edge < 2) ? (edge == 0 ? v0 : v1) : v0 + f * (v1 - v0);
            std::vector<std::vector<Vec3d>> ders;
            surface.Derivatives(u, v, 1, &ders);
            if (ders.size() < 2 || ders[0].size() < 2) continue;
            if (ders[1][0].Cross(ders[0][1]).Length() <= scale * 1e-9) return true;
        }
    }
    return false;
}

// A deep copy of one body into another model, optionally with every
// face's orientation flipped and the shell marked as bounding a void.
// Needed by the containment shortcuts: when neither solid cuts the other,
// the answer is one of the inputs, possibly turned inside out to become a
// cavity in the other.
EntityId CopyBody(const Model &src, EntityId body_id, Model *out, bool reverse, bool as_void,
                  const std::string &name) {
    const Body *body = src.GetBody(body_id);
    if (body == nullptr) return kNoEntity;
    std::map<EntityId, EntityId> vertex_map;
    std::map<EntityId, EntityId> edge_map;
    auto copy_vertex = [&](EntityId v) {
        const auto found = vertex_map.find(v);
        if (found != vertex_map.end()) return found->second;
        const Vertex *vertex = src.GetVertex(v);
        const EntityId id = vertex != nullptr ? out->AddVertex(vertex->point, vertex->tolerance) : kNoEntity;
        vertex_map.emplace(v, id);
        return id;
    };
    auto copy_edge = [&](EntityId e) {
        const auto found = edge_map.find(e);
        if (found != edge_map.end()) return found->second;
        const Edge *edge = src.GetEdge(e);
        EntityId id = kNoEntity;
        if (edge != nullptr) {
            const Curve3 *curve = src.CurveAt(edge->curve);
            if (curve != nullptr) {
                const int index = out->AddCurve(std::shared_ptr<const Curve3>(curve->Clone().release()));
                id = out->AddEdge(index, copy_vertex(edge->start_vertex), copy_vertex(edge->end_vertex),
                                  edge->t_start, edge->t_end, edge->tolerance);
            }
        }
        edge_map.emplace(e, id);
        return id;
    };
    std::vector<EntityId> out_shells;
    for (EntityId s : body->shells) {
        const Shell *shell = src.GetShell(s);
        if (shell == nullptr) continue;
        std::vector<EntityId> out_faces;
        for (EntityId f : shell->faces) {
            const Face *face = src.GetFace(f);
            const Surface *surface = src.SurfaceAt(face != nullptr ? face->surface : -1);
            if (face == nullptr || surface == nullptr) continue;
            std::vector<EntityId> out_loops;
            for (EntityId l : face->loops) {
                const Loop *loop = src.GetLoop(l);
                if (loop == nullptr) continue;
                std::vector<EntityId> out_coedges;
                for (EntityId c : loop->coedges) {
                    const CoEdge *coedge = src.GetCoEdge(c);
                    if (coedge == nullptr) continue;
                    out_coedges.push_back(out->AddCoEdge(copy_edge(coedge->edge), coedge->orientation));
                }
                out_loops.push_back(out->AddLoop(out_coedges, loop->is_outer));
            }
            const int index = out->AddSurface(std::shared_ptr<const Surface>(surface->Clone().release()));
            const Orientation orientation =
                reverse ? Flip(face->orientation) : face->orientation;
            out_faces.push_back(out->AddFace(index, orientation, out_loops, face->name, face->tolerance));
        }
        out_shells.push_back(out->AddShell(out_faces, as_void ? false : shell->is_outer));
    }
    return out->AddBody(out_shells, name);
}

// A point on a body's boundary, used only to ask where that body sits
// relative to another one.
Vec3d PointOnBody(const Model &model, const std::vector<EntityId> &faces) {
    for (EntityId f : faces) {
        for (EntityId e : model.EdgesOfFace(f)) {
            return model.EdgeStartPoint(e);
        }
    }
    return Vec3d{};
}

// Where an intersection curve crosses the seam of a periodic face, as a
// list of sub-ranges of its own parameter.
//
// A cylinder's parameter space is a rectangle with its two vertical edges
// glued, and a curve that runs all the way round crosses that glue. Its
// p-curve is built by projecting and unwrapping for continuity (Part
// C.1's BuildPCurveOnSurface), which is right -- a p-curve that jumped by
// a full turn in the middle would be useless -- but it means the curve
// comes back running outside the surface's stated domain. The arrangement
// then cannot see that it meets the face's own seam edges, the face comes
// out uncut, and the boolean quietly returns the solid unchanged.
//
// MakeCylinder's tube never showed this because the analytic plane/
// cylinder intersection happens to build its circle on the cylinder's own
// frame, so the p-curve starts exactly at the seam and runs exactly to
// it. A tube built by extruding a sketched circle has no reason to line
// up that way, and does not.
std::vector<std::pair<double, double>> SeamSplitRanges(const Surface &surface, const Curve3 &curve,
                                                      double t0, double t1,
                                                      const IntersectOptions &options) {
    std::vector<std::pair<double, double>> ranges{{t0, t1}};
    double u_lo = 0.0;
    double u_hi = 0.0;
    double v_lo = 0.0;
    double v_hi = 0.0;
    surface.Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    const double period_u = surface.IsClosedU(options.tolerance) ? (u_hi - u_lo) : 0.0;
    const double period_v = surface.IsClosedV(options.tolerance) ? (v_hi - v_lo) : 0.0;
    if (period_u <= 0.0 && period_v <= 0.0) return ranges;

    auto unwrap = [](double value, double reference, double period) {
        if (period <= 0.0) return value;
        return value - period * std::round((value - reference) / period);
    };
    // Which copy of the parameter rectangle a point has landed in. It
    // changes by one exactly when the curve crosses a seam.
    auto band = [&](double u, double v) {
        const long long bu = period_u > 0.0 ? static_cast<long long>(std::floor((u - u_lo) / period_u)) : 0;
        const long long bv = period_v > 0.0 ? static_cast<long long>(std::floor((v - v_lo) / period_v)) : 0;
        return std::make_pair(bu, bv);
    };
    const int samples = std::max(64, options.samples);
    std::vector<double> parameters;
    std::vector<Vec2d> unwrapped;
    Vec2d previous;
    bool have_previous = false;
    for (int i = 0; i <= samples; ++i) {
        const double t = t0 + (t1 - t0) * static_cast<double>(i) / static_cast<double>(samples);
        double u = 0.0;
        double v = 0.0;
        if (!surface.ClosestPoint(curve.Point(t), &u, &v, nullptr, options.tolerance)) return ranges;
        if (have_previous) {
            u = unwrap(u, previous.x, period_u);
            v = unwrap(v, previous.y, period_v);
        }
        previous = Vec2d{u, v};
        have_previous = true;
        parameters.push_back(t);
        unwrapped.push_back(previous);
    }

    std::vector<double> cuts;
    for (std::size_t i = 1; i < unwrapped.size(); ++i) {
        if (band(unwrapped[i - 1].x, unwrapped[i - 1].y) == band(unwrapped[i].x, unwrapped[i].y)) continue;
        // Bisect for the crossing, so the two pieces meet on the seam
        // rather than a sample interval either side of it.
        double inside = parameters[i - 1];
        double outside = parameters[i];
        const auto start_band = band(unwrapped[i - 1].x, unwrapped[i - 1].y);
        for (int step = 0; step < 50; ++step) {
            const double middle = 0.5 * (inside + outside);
            double u = 0.0;
            double v = 0.0;
            if (!surface.ClosestPoint(curve.Point(middle), &u, &v, nullptr, options.tolerance)) break;
            u = unwrap(u, unwrapped[i - 1].x, period_u);
            v = unwrap(v, unwrapped[i - 1].y, period_v);
            if (band(u, v) == start_band) {
                inside = middle;
            } else {
                outside = middle;
            }
        }
        cuts.push_back(0.5 * (inside + outside));
    }
    if (cuts.empty()) return ranges;

    ranges.clear();
    double from = t0;
    for (double cut : cuts) {
        if (std::fabs(cut - from) > std::fabs(t1 - t0) * 1e-9) ranges.push_back({from, cut});
        from = cut;
    }
    if (std::fabs(t1 - from) > std::fabs(t1 - t0) * 1e-9) ranges.push_back({from, t1});
    return ranges;
}

// Moves a p-curve into the surface's own parameter rectangle.
//
// A curve cut at the seam produces one piece either side, and the second
// of them is unwrapped from wherever ClosestPoint happened to land its
// first sample -- which at the seam is genuinely ambiguous, and comes
// back as the copy of the rectangle next door about half the time. The
// piece is then perfectly shaped and in the wrong place: it does not meet
// the face's boundary, the arrangement drops it for having nothing to
// connect to, and the face comes out uncut with no error anywhere.
std::shared_ptr<const Curve3> ShiftIntoDomain(const Surface &surface, std::shared_ptr<const Curve3> pcurve,
                                              const IntersectOptions &options) {
    double u_lo = 0.0;
    double u_hi = 0.0;
    double v_lo = 0.0;
    double v_hi = 0.0;
    surface.Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    const double period_u = surface.IsClosedU(options.tolerance) ? (u_hi - u_lo) : 0.0;
    const double period_v = surface.IsClosedV(options.tolerance) ? (v_hi - v_lo) : 0.0;
    if (period_u <= 0.0 && period_v <= 0.0) return pcurve;
    double lo = 0.0;
    double hi = 0.0;
    pcurve->Domain(&lo, &hi);
    // Judged by the middle of the piece rather than an end: an end can sit
    // exactly on the seam, where which side it belongs to is the very
    // question being asked.
    const Vec3d middle = pcurve->Point(0.5 * (lo + hi));
    double shift_u = 0.0;
    double shift_v = 0.0;
    if (period_u > 0.0) shift_u = -period_u * std::round((middle.x - 0.5 * (u_lo + u_hi)) / period_u);
    if (period_v > 0.0) shift_v = -period_v * std::round((middle.y - 0.5 * (v_lo + v_hi)) / period_v);
    if (shift_u == 0.0 && shift_v == 0.0) return pcurve;
    std::shared_ptr<Curve3> moved(pcurve->Clone().release());
    moved->Transform(Mat4d::Translation(Vec3d{shift_u, shift_v, 0.0}));
    return moved;
}

// One piece a face falls into: the surface it lies on, its outer cycle,
// and any cycles bounding holes in it.
struct FacePiece {
    const Surface *surface = nullptr;
    std::shared_ptr<const Surface> surface_owner;
    Orientation orientation = Orientation::Forward;
    std::vector<Segment> outer;
    std::vector<std::vector<Segment>> holes;
    // Several points in the piece's interior, deepest first. Classifying
    // takes the first that is not on the other solid's boundary.
    std::vector<Vec3d> sample_points;
    bool from_a = true;
    std::string name;
};

}  // namespace

// ---------------------------------------------------------------------

bool BooleanOperation(const Model &a_model, EntityId a_body, const Model &b_model, EntityId b_body,
                      BooleanOp op, Model *out, BooleanReport *report, const BooleanOptions &options) {
    report->ok = false;
    const Body *ba = a_model.GetBody(a_body);
    const Body *bb = b_model.GetBody(b_body);
    if (ba == nullptr || bb == nullptr) {
        report->error = "one of the bodies does not exist";
        return false;
    }

    auto faces_of = [](const Model &model, const Body &body) {
        std::vector<EntityId> faces;
        for (EntityId s : body.shells) {
            const Shell *shell = model.GetShell(s);
            if (shell == nullptr) continue;
            faces.insert(faces.end(), shell->faces.begin(), shell->faces.end());
        }
        return faces;
    };
    const std::vector<EntityId> a_faces = faces_of(a_model, *ba);
    const std::vector<EntityId> b_faces = faces_of(b_model, *bb);

    // Refuse a pole before doing any work, with a message that says what
    // is wrong -- see HasParameterPole for why these faces cannot be cut.
    auto check_poles = [&](const Model &model, const std::vector<EntityId> &faces, const char *which) {
        for (EntityId f : faces) {
            const Face *face = model.GetFace(f);
            const Surface *surface = model.SurfaceAt(face != nullptr ? face->surface : -1);
            if (surface != nullptr && HasParameterPole(*surface)) {
                report->error = std::string("the ") + which +
                                " solid has a face whose parameterization has a pole (a sphere's poles, a "
                                "cone's apex, a revolution meeting its axis); cutting such a face is not "
                                "implemented -- see cad_boolean.h";
                return false;
            }
        }
        return true;
    };
    if (!check_poles(a_model, a_faces, "first")) return false;
    if (!check_poles(b_model, b_faces, "second")) return false;

    // --- Imprint, step one: the curves ---------------------------------
    //
    // Every face pair whose bounds overlap is intersected, and each branch
    // is trimmed to the runs that lie on *both* faces -- an intersection
    // of two infinite surfaces says nothing about whether the curve is
    // inside either face's trimming loops.
    struct CurveOnFaces {
        std::shared_ptr<const Curve3> curve;
        double t0 = 0.0;
        double t1 = 1.0;
        EntityId face_a = kNoEntity;
        EntityId face_b = kNoEntity;
    };
    std::vector<CurveOnFaces> curves;

    for (EntityId fa : a_faces) {
        const Face *face_a = a_model.GetFace(fa);
        const Surface *surface_a = a_model.SurfaceAt(face_a != nullptr ? face_a->surface : -1);
        if (surface_a == nullptr) continue;
        for (EntityId fb : b_faces) {
            const Face *face_b = b_model.GetFace(fb);
            const Surface *surface_b = b_model.SurfaceAt(face_b != nullptr ? face_b->surface : -1);
            if (surface_b == nullptr) continue;
            if (!surface_a->Bounds().Overlaps(surface_b->Bounds(), options.tolerance.linear)) continue;

            const SurfaceIntersection isect = IntersectSurfaces(*surface_a, *surface_b, options.intersect);
            if (isect.kind == ContactKind::Coincident) {
                report->error =
                    "the two solids share a coincident face; merging those needs a separate code path that "
                    "this implementation does not have (see cad_boolean.h)";
                return false;
            }
            if (isect.branches.empty()) continue;
            ++report->face_pairs_intersected;

            for (const IntersectionBranch &branch : isect.branches) {
                double lo = 0.0;
                double hi = 0.0;
                branch.curve->Domain(&lo, &hi);
                // Walk the branch and keep the runs that lie on both
                // faces.
                //
                // Where a run begins and ends is found by bisection, not
                // read off the sample grid. Two things depend on it. The
                // cut has to reach the face's boundary exactly, or it
                // leaves a slit the arrangement can walk around instead
                // of a cut that separates the face -- and a slit shows up
                // as a region of the full, uncut area, not as an error.
                // And the two faces that meet along the cut have to agree
                // on where it ends to better than their own tolerance,
                // or the stitch finds two edges where there is one.
                //
                // The sample count is also generous rather than minimal,
                // because a run shorter than one sample interval is not
                // refined, it is never seen: a corner overlap 0.5 across
                // on a branch spanning tens of units went missing that
                // way, on both faces at once, and the only symptom was a
                // shell that would not close.
                // On a face means on its surface *and* inside its
                // boundary -- and the first half has to be checked.
                // ClosestPoint clamps to the surface's parameter range
                // rather than failing, so a point beyond a surface's own
                // extent comes back sitting on that extent's edge, which
                // the point-in-face test then decides by round-off. A
                // surface trimmed to exactly its face -- which an
                // extruded side face is -- makes that the common case
                // rather than the exotic one, and the branch gets trimmed
                // to a run that was never on the face at all.
                auto lands_on = [&](const Surface &surface, const Model &model, EntityId face,
                                    const Vec3d &p) {
                    double u = 0.0;
                    double v = 0.0;
                    if (!surface.ClosestPoint(p, &u, &v, nullptr, options.tolerance)) return false;
                    if (!options.tolerance.SamePoint(surface.Point(u, v), p)) return false;
                    return PointInFace(model, face, u, v);
                };
                auto on_both = [&](double t) {
                    const Vec3d p = branch.curve->Point(t);
                    return lands_on(*surface_a, a_model, fa, p) && lands_on(*surface_b, b_model, fb, p);
                };
                // The parameter of the transition between an inside and
                // an outside sample, to the curve's own tolerance.
                auto bisect = [&](double inside, double outside) {
                    for (int step = 0; step < 60; ++step) {
                        const double middle = 0.5 * (inside + outside);
                        if ((branch.curve->Point(inside) - branch.curve->Point(outside)).Length() <=
                            options.tolerance.linear * 0.01) {
                            break;
                        }
                        if (on_both(middle)) {
                            inside = middle;
                        } else {
                            outside = middle;
                        }
                    }
                    return inside;
                };
                const int samples = std::max(256, options.curve_samples * 4);
                const double step = (hi - lo) / static_cast<double>(samples);
                int run_start = -1;
                for (int i = 0; i <= samples; ++i) {
                    const double t = lo + step * static_cast<double>(i);
                    const bool inside = on_both(t);
                    if (inside && run_start < 0) run_start = i;
                    if ((!inside || i == samples) && run_start >= 0) {
                        const int run_end = inside ? i : (i - 1);
                        CurveOnFaces entry;
                        entry.curve = branch.curve;
                        entry.t0 = lo + step * static_cast<double>(run_start);
                        entry.t1 = lo + step * static_cast<double>(run_end);
                        // Push each end out to where the branch actually
                        // leaves the two faces. A run of a single sample
                        // is kept rather than discarded -- it is a real
                        // crossing, just a short one, and after
                        // refinement it has its true length.
                        if (run_start > 0) entry.t0 = bisect(entry.t0, entry.t0 - step);
                        if (run_end < samples) entry.t1 = bisect(entry.t1, entry.t1 + step);
                        // Long enough to be a cut rather than a touch.
                        // Measured through the midpoint, because a run
                        // that covers a closed branch has the *same*
                        // point at both ends -- an intersection circle
                        // lying wholly inside both faces is the common
                        // case, not a corner one, and an end-to-end
                        // length test throws every one of them away.
                        const Vec3d start = branch.curve->Point(entry.t0);
                        const Vec3d middle = branch.curve->Point(0.5 * (entry.t0 + entry.t1));
                        const Vec3d finish = branch.curve->Point(entry.t1);
                        if (std::max((middle - start).Length(), (finish - middle).Length()) >
                            options.tolerance.linear) {
                            entry.face_a = fa;
                            entry.face_b = fb;
                            curves.push_back(entry);
                        }
                        run_start = -1;
                    }
                }
            }
        }
    }
    report->intersection_curves = static_cast<int>(curves.size());
    if (curves.empty()) {
        // No face of one solid cuts a face of the other. That is three
        // different situations, not one, and they are told apart by
        // asking where a point of each solid lies in the other: the
        // solids are disjoint, or one is wholly inside the other. Each
        // answer is one of the inputs copied out, so there is nothing to
        // imprint, classify or stitch -- but getting here and reporting
        // "they do not intersect" would be wrong for the contained case,
        // where the answer is perfectly well defined.
        const bool b_in_a = ClassifyPoint(a_model, a_body, PointOnBody(b_model, b_faces),
                                          options.classify) == PointClass::Inside;
        const bool a_in_b = ClassifyPoint(b_model, b_body, PointOnBody(a_model, a_faces),
                                          options.classify) == PointClass::Inside;
        switch (op) {
            case BooleanOp::Union:
                if (b_in_a) report->body = CopyBody(a_model, a_body, out, false, false, "boolean");
                if (a_in_b) report->body = CopyBody(b_model, b_body, out, false, false, "boolean");
                break;
            case BooleanOp::Intersection:
                if (b_in_a) report->body = CopyBody(b_model, b_body, out, false, false, "boolean");
                if (a_in_b) report->body = CopyBody(a_model, a_body, out, false, false, "boolean");
                break;
            case BooleanOp::Difference:
                if (!b_in_a && !a_in_b) {
                    // Disjoint: A is untouched by removing something that
                    // never met it.
                    report->body = CopyBody(a_model, a_body, out, false, false, "boolean");
                } else if (b_in_a) {
                    // B is wholly inside A, so A - B is A with a cavity:
                    // one outer shell and one inner shell bounding the
                    // void, the latter turned inside out so its faces
                    // point into the material.
                    const EntityId copied = CopyBody(a_model, a_body, out, false, false, "boolean");
                    const EntityId void_body = CopyBody(b_model, b_body, out, true, true, "");
                    const Body *outer = out->GetBody(copied);
                    const Body *inner = out->GetBody(void_body);
                    if (outer != nullptr && inner != nullptr) {
                        std::vector<EntityId> shells = outer->shells;
                        shells.insert(shells.end(), inner->shells.begin(), inner->shells.end());
                        report->body = out->AddBody(shells, "boolean");
                    }
                }
                // a_in_b with no crossing means A is wholly consumed and
                // the result is empty, which is not a body; left refused.
                break;
        }
        if (report->body == kNoEntity) {
            report->error =
                a_in_b || b_in_a
                    ? "one solid lies wholly inside the other and this operation leaves nothing behind; an "
                      "empty result is not a body"
                    : "the two solids are disjoint, so this operation would leave two separate lumps; a body "
                      "with two unconnected outer shells is not something this returns";
            return false;
        }
        std::string pcurve_error;
        PCurveOptions pcurve_options;
        pcurve_options.tolerance = options.tolerance;
        if (!BuildAllPCurves(out, pcurve_options, &pcurve_error)) {
            report->error = "could not build p-curves on the result: " + pcurve_error;
            return false;
        }
        report->ok = true;
        return true;
    }

    // --- Imprint, step two: cut each face ------------------------------
    std::vector<FacePiece> pieces;
    auto imprint_body = [&](const Model &model, const std::vector<EntityId> &faces, bool from_a) {
        for (EntityId face_id : faces) {
            const Face *face = model.GetFace(face_id);
            if (face == nullptr) continue;
            const Surface *surface = model.SurfaceAt(face->surface);
            if (surface == nullptr) continue;

            // Every segment this face's arrangement will work on, kept
            // here so a region's boundary can be traced back to the 3D
            // geometry it came from. The arrangement itself reasons only
            // in parameter space and carries the index as a tag.
            std::vector<Segment> segments;
            Arrangement arrangement;
            auto add = [&](const Segment &segment) {
                ArrangementSegment entry;
                entry.curve = segment.pcurve;
                entry.t0 = segment.p0;
                entry.t1 = segment.p1;
                entry.tag = static_cast<int>(segments.size());
                segments.push_back(segment);
                arrangement.AddSegment(entry);
            };
            // The face's own boundary.
            for (EntityId loop_id : face->loops) {
                const Loop *loop = model.GetLoop(loop_id);
                if (loop == nullptr) continue;
                for (EntityId coedge_id : loop->coedges) {
                    const CoEdge *coedge = model.GetCoEdge(coedge_id);
                    if (coedge == nullptr) continue;
                    const Curve3 *pcurve = model.PCurveAt(coedge->pcurve);
                    const Edge *edge = model.GetEdge(coedge->edge);
                    if (pcurve == nullptr || edge == nullptr) continue;
                    const Curve3 *curve = model.CurveAt(edge->curve);
                    if (curve == nullptr) continue;
                    Segment segment;
                    double lo = 0.0;
                    double hi = 0.0;
                    pcurve->Domain(&lo, &hi);
                    segment.pcurve = std::shared_ptr<const Curve3>(pcurve, [](const Curve3 *) {});
                    segment.p0 = lo;
                    segment.p1 = hi;
                    segment.curve3d = std::shared_ptr<const Curve3>(curve, [](const Curve3 *) {});
                    segment.c0 = edge->t_start;
                    segment.c1 = edge->t_end;
                    add(segment);
                }
            }
            // The intersection curves that land on this face.
            for (const CurveOnFaces &entry : curves) {
                if ((from_a ? entry.face_a : entry.face_b) != face_id) continue;
                // Cut at the face's seam first, so that no piece has to
                // be described by a p-curve running outside the surface's
                // own parameter rectangle.
                for (const std::pair<double, double> &range :
                     SeamSplitRanges(*surface, *entry.curve, entry.t0, entry.t1, options.intersect)) {
                    std::shared_ptr<const Curve3> pcurve;
                    if (!BuildPCurveOnSurface(*surface, *entry.curve, range.first, range.second,
                                              options.intersect, &pcurve)) {
                        continue;
                    }
                    pcurve = ShiftIntoDomain(*surface, pcurve, options.intersect);
                    Segment segment;
                    double lo = 0.0;
                    double hi = 0.0;
                    pcurve->Domain(&lo, &hi);
                    segment.pcurve = pcurve;
                    segment.p0 = lo;
                    segment.p1 = hi;
                    segment.curve3d = entry.curve;
                    segment.c0 = range.first;
                    segment.c1 = range.second;
                    segment.from_intersection = true;
                    add(segment);
                }
            }
            if (!arrangement.Build(options.curve_samples)) {
                if (std::getenv("MEP_BOOL_DEBUG")) std::fprintf(stderr, "    BUILD FAILED\n");
                continue;
            }

            // Turning one cycle back into 3D segments. The arrangement
            // split the p-curves wherever they crossed, and each piece's
            // 3D parameter range has to be recovered from its new
            // parameter-space range -- the two curves were fitted
            // independently and do not share a parameterization, so the
            // correspondence goes through geometry.
            auto rebuild = [&](int cycle, std::vector<Segment> *out) {
                for (const ArrangementSegment &piece : arrangement.CycleSegments(cycle)) {
                    if (piece.tag < 0 || static_cast<std::size_t>(piece.tag) >= segments.size()) return false;
                    Segment segment = segments[static_cast<std::size_t>(piece.tag)];
                    segment.p0 = piece.t0;
                    segment.p1 = piece.t1;
                    // Walked along the piece rather than read off its two
                    // ends.
                    //
                    // Inverting a point on a *closed* curve is ambiguous
                    // at its seam: the start of the circle and its end
                    // are the same place, and ClosestPoint may answer
                    // with either parameter. Taking each end on its own
                    // therefore produces, about half the time, a range
                    // that runs the long way round -- a quarter of a rim
                    // came back claiming to be the other three quarters,
                    // so two faces of one hole claimed the same edge and
                    // a third edge went unused. Following the piece
                    // through its middle settles it, and the result is
                    // then slid back into the curve's own domain, since a
                    // range shifted by a whole turn describes the same
                    // arc but would not match the other face's copy of it.
                    double lo = 0.0;
                    double hi = 0.0;
                    segment.curve3d->Domain(&lo, &hi);
                    const bool closed =
                        (segment.curve3d->Point(lo) - segment.curve3d->Point(hi)).Length() <= 1e-9;
                    const double period = closed ? (hi - lo) : 0.0;
                    constexpr int kWalk = 8;
                    double previous = 0.0;
                    double first = 0.0;
                    double last = 0.0;
                    for (int k = 0; k <= kWalk; ++k) {
                        const double at =
                            segment.p0 + (segment.p1 - segment.p0) * static_cast<double>(k) / kWalk;
                        double value = 0.0;
                        if (!CorrespondingParameter(segment, *surface, at, &value)) return false;
                        if (k > 0 && period > 0.0) {
                            value -= period * std::round((value - previous) / period);
                        }
                        previous = value;
                        if (k == 0) first = value;
                        last = value;
                    }
                    if (period > 0.0) {
                        const double shift = period * std::floor((std::min(first, last) - lo) / period);
                        first -= shift;
                        last -= shift;
                    }
                    segment.c0 = first;
                    segment.c1 = last;
                    out->push_back(segment);
                }
                return true;
            };

            for (const ArrangementRegion &region : arrangement.Regions(4)) {
                const std::vector<Vec2d> &interiors = region.interior;
                const Vec2d interior = interiors.front();
                // Regions of the arrangement that fall outside the
                // original face are discarded -- they are artefacts of
                // cutting, not parts of the solid.
                if (!PointInFace(model, face_id, interior.x, interior.y)) {
                    continue;
                }
                std::vector<Segment> outer_segments;
                if (!rebuild(region.outer_cycle, &outer_segments)) continue;
                std::vector<std::vector<Segment>> hole_segments;
                bool holes_ok = true;
                for (int hole : region.hole_cycles) {
                    std::vector<Segment> hole_out;
                    if (!rebuild(hole, &hole_out)) {
                        holes_ok = false;
                        break;
                    }
                    hole_segments.push_back(std::move(hole_out));
                }
                if (!holes_ok) continue;

                FacePiece piece;
                piece.surface = surface;
                piece.orientation = face->orientation;
                for (const Vec2d &uv : interiors) {
                    if (!PointInFace(model, face_id, uv.x, uv.y)) continue;
                    piece.sample_points.push_back(surface->Point(uv.x, uv.y));
                }
                piece.from_a = from_a;
                piece.name = face->name;
                piece.outer = std::move(outer_segments);
                piece.holes = std::move(hole_segments);
                pieces.push_back(std::move(piece));
            }
        }
    };
    imprint_body(a_model, a_faces, true);
    const int after_a = static_cast<int>(pieces.size());
    imprint_body(b_model, b_faces, false);
    report->pieces_from_a = after_a;
    report->pieces_from_b = static_cast<int>(pieces.size()) - after_a;

    // --- Classify and select -------------------------------------------
    std::vector<FacePiece> kept;
    for (FacePiece &piece : pieces) {
        const Model &other_model = piece.from_a ? b_model : a_model;
        const EntityId other_body = piece.from_a ? b_body : a_body;
        PointClass where = PointClass::Boundary;
        for (const Vec3d &sample : piece.sample_points) {
            where = ClassifyPoint(other_model, other_body, sample, options.classify);
            if (where != PointClass::Boundary) break;
        }
        // Every point tried lands on the other solid's boundary, which is
        // what a genuinely coincident face looks like. Those are refused
        // up front, so reaching here means the piece cannot be placed and
        // is left out rather than guessed at.
        if (where == PointClass::Boundary) continue;
        const bool inside = (where == PointClass::Inside);

        bool keep = false;
        bool flip = false;
        switch (op) {
            case BooleanOp::Union:
                keep = !inside;
                break;
            case BooleanOp::Intersection:
                keep = inside;
                break;
            case BooleanOp::Difference:
                // A's outside of B, plus B's inside of A -- and the
                // latter reversed, because it now bounds the cavity
                // rather than the solid it came from.
                keep = piece.from_a ? !inside : inside;
                flip = !piece.from_a;
                break;
        }
        if (!keep) continue;
        if (flip) piece.orientation = Flip(piece.orientation);
        kept.push_back(piece);
    }
    report->pieces_kept = static_cast<int>(kept.size());
    if (kept.empty()) {
        report->error = "no face pieces survived classification; the result would be empty";
        return false;
    }

    // --- Harmonise the splits -------------------------------------------
    //
    // Both faces along an intersection curve carry segments of the *same*
    // Curve3, but not necessarily the same segments of it. A cylinder's
    // lateral face has a seam, so the circle where it meets a plane
    // arrives there as two arcs, while on the plane it is one closed
    // curve. The stitch matches edges by their endpoints, so those two
    // descriptions of one edge never meet: every edge on both faces comes
    // out used once, and the shell is open along the whole circle.
    //
    // The fix is to make every piece agree on where the curve is cut.
    // Each parameter at which any piece breaks the curve becomes a break
    // for all of them, so the two faces describe the seam-split circle
    // the same way and the existing endpoint match finds it.
    {
        std::map<const Curve3 *, std::vector<double>> breaks;
        auto gather = [&](const std::vector<Segment> &loop) {
            for (const Segment &segment : loop) {
                std::vector<double> &list = breaks[segment.curve3d.get()];
                list.push_back(segment.c0);
                list.push_back(segment.c1);
            }
        };
        for (const FacePiece &piece : kept) {
            gather(piece.outer);
            for (const std::vector<Segment> &hole : piece.holes) gather(hole);
        }
        // Breakpoints are merged by *distance in space*, not by
        // closeness in parameter. The same corner is computed
        // independently on each of the two faces that meet there, and
        // the two answers agree to a tolerance, not to the last bit --
        // a parameter-space epsilon tight enough to be meaningful
        // therefore leaves the two as distinct breaks, and the segment
        // between them becomes a sliver a few nanometres long that
        // exists on one face and not the other. The shell is then open
        // along it, which is how six edges of a box/box union came out
        // used once.
        const double merge_distance = std::max(1e-12, options.tolerance.linear);
        auto point_at = [](const Curve3 *curve, double t) { return curve->Point(t); };
        for (auto &entry : breaks) {
            const Curve3 *curve = entry.first;
            std::vector<double> &list = entry.second;
            std::sort(list.begin(), list.end());
            std::vector<double> unique;
            for (double t : list) {
                if (unique.empty() ||
                    (point_at(curve, t) - point_at(curve, unique.back())).Length() > merge_distance) {
                    unique.push_back(t);
                }
            }
            list = std::move(unique);
        }
        auto resplit = [&](std::vector<Segment> &loop) {
            std::vector<Segment> rebuilt;
            for (const Segment &segment : loop) {
                const Curve3 *curve = segment.curve3d.get();
                const std::vector<double> &list = breaks[curve];
                const double lo = std::min(segment.c0, segment.c1);
                const double hi = std::max(segment.c0, segment.c1);
                const Vec3d lo_point = point_at(curve, lo);
                const Vec3d hi_point = point_at(curve, hi);
                std::vector<double> cuts{lo};
                for (double t : list) {
                    if (t <= lo || t >= hi) continue;
                    const Vec3d point = point_at(curve, t);
                    if ((point - lo_point).Length() <= merge_distance) continue;
                    if ((point - hi_point).Length() <= merge_distance) continue;
                    cuts.push_back(t);
                }
                cuts.push_back(hi);
                if (cuts.size() == 2) {
                    rebuilt.push_back(segment);
                    continue;
                }
                // The segment runs c0 -> c1, which may descend; the cuts
                // were gathered ascending, so they are walked the way the
                // segment goes. The p-curve range is carried along
                // proportionally -- it is only the arrangement's input and
                // is not read again after this, but leaving it describing
                // the whole span would make the struct lie.
                const bool ascending = segment.c1 >= segment.c0;
                for (std::size_t i = 0; i + 1 < cuts.size(); ++i) {
                    const std::size_t k = ascending ? i : cuts.size() - 2 - i;
                    Segment part = segment;
                    part.c0 = ascending ? cuts[k] : cuts[k + 1];
                    part.c1 = ascending ? cuts[k + 1] : cuts[k];
                    const double f0 = (part.c0 - segment.c0) / (segment.c1 - segment.c0);
                    const double f1 = (part.c1 - segment.c0) / (segment.c1 - segment.c0);
                    part.p0 = segment.p0 + f0 * (segment.p1 - segment.p0);
                    part.p1 = segment.p0 + f1 * (segment.p1 - segment.p0);
                    rebuilt.push_back(part);
                }
            }
            loop = std::move(rebuilt);
        };
        for (FacePiece &piece : kept) {
            resplit(piece.outer);
            for (std::vector<Segment> &hole : piece.holes) resplit(hole);
        }
    }

    // --- Stitch ---------------------------------------------------------
    //
    // Faces are rebuilt in the output model, with vertices and edges
    // shared between pieces that meet. Matching is geometric: two edges
    // are the same edge when their end points and mid point coincide,
    // in either direction.
    std::map<std::tuple<long long, long long, long long>, EntityId> vertex_map;
    auto quantize = [&](const Vec3d &p) {
        const double q = 1.0 / std::max(1e-12, options.tolerance.linear * 10.0);
        return std::make_tuple(std::llround(p.x * q), std::llround(p.y * q), std::llround(p.z * q));
    };
    auto vertex_for = [&](const Vec3d &p) {
        const auto key = quantize(p);
        const auto found = vertex_map.find(key);
        if (found != vertex_map.end()) return found->second;
        const EntityId id = out->AddVertex(p, options.tolerance.linear);
        vertex_map.emplace(key, id);
        return id;
    };

    struct EdgeKey {
        std::tuple<long long, long long, long long> a;
        std::tuple<long long, long long, long long> b;
        std::tuple<long long, long long, long long> mid;
        bool operator<(const EdgeKey &other) const {
            return std::tie(a, b, mid) < std::tie(other.a, other.b, other.mid);
        }
    };
    std::map<EdgeKey, std::pair<EntityId, bool>> edge_map;  // edge id, and whether it runs a->b

    std::vector<EntityId> out_faces;
    for (const FacePiece &piece : kept) {
        auto build_loop = [&](const std::vector<Segment> &segments, bool is_outer) {
            std::vector<EntityId> coedges;
            for (const Segment &segment : segments) {
                const Vec3d start = segment.curve3d->Point(segment.c0);
                const Vec3d end = segment.curve3d->Point(segment.c1);
                const Vec3d middle = segment.curve3d->Point(0.5 * (segment.c0 + segment.c1));
                EdgeKey key;
                key.a = quantize(start);
                key.b = quantize(end);
                key.mid = quantize(middle);
                bool forward = true;
                if (key.b < key.a) {
                    std::swap(key.a, key.b);
                    forward = false;
                }
                EntityId edge_id = kNoEntity;
                const auto found = edge_map.find(key);
                if (found != edge_map.end()) {
                    edge_id = found->second.first;
                } else {
                    const int curve_index =
                        out->AddCurve(std::shared_ptr<const Curve3>(segment.curve3d->Clone().release()));
                    const EntityId v0 = vertex_for(forward ? start : end);
                    const EntityId v1 = vertex_for(forward ? end : start);
                    edge_id = out->AddEdge(curve_index, v0, v1, forward ? segment.c0 : segment.c1,
                                           forward ? segment.c1 : segment.c0, options.tolerance.linear);
                    edge_map.emplace(key, std::make_pair(edge_id, forward));
                }
                const Edge *edge = out->GetEdge(edge_id);
                const bool runs_forward =
                    edge != nullptr && options.tolerance.SamePoint(out->EdgeStartPoint(edge_id), start);
                coedges.push_back(
                    out->AddCoEdge(edge_id, runs_forward ? Orientation::Forward : Orientation::Reversed));
            }
            return out->AddLoop(coedges, is_outer);
        };
        std::vector<EntityId> loops;
        loops.push_back(build_loop(piece.outer, true));
        for (const std::vector<Segment> &hole : piece.holes) loops.push_back(build_loop(hole, false));
        const int surface_index =
            out->AddSurface(std::shared_ptr<const Surface>(piece.surface->Clone().release()));
        out_faces.push_back(out->AddFace(surface_index, piece.orientation, loops, piece.name,
                                          options.tolerance.linear));
    }

    const EntityId shell = out->AddShell(out_faces, true);
    report->body = out->AddBody({shell}, "boolean");

    std::string error;
    PCurveOptions pcurve_options;
    pcurve_options.tolerance = options.tolerance;
    if (!BuildAllPCurves(out, pcurve_options, &error)) {
        report->error = "could not build p-curves on the result: " + error;
        return false;
    }
    if (options.validate_result) {
        ValidationReport validation;
        ValidationOptions validation_options;
        validation_options.tolerance = options.tolerance;
        if (!ValidateBody(*out, report->body, &validation, validation_options)) {
            report->error = "the result is not a valid solid:\n" + validation.Summary();
            return false;
        }
    }
    report->ok = true;
    return true;
}

bool CopyBodyInto(const Model &source, EntityId body, Model *out, EntityId *out_body, bool reverse,
                  bool as_void, const std::string &name, std::string *error) {
    const EntityId copied = CopyBody(source, body, out, reverse, as_void, name);
    if (copied == kNoEntity) {
        if (error != nullptr) *error = "no such body to copy";
        return false;
    }
    *out_body = copied;
    return true;
}

}  // namespace cad
