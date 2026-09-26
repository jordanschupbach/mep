#include "cad_feature.h"

#include "cad_naming.h"
#include "cad_pcurve.h"
#include "cad_tessellate.h"
#include "cad_validate.h"

#include <algorithm>
#include <cmath>
#include <utility>

namespace cad {
namespace {

// One piece of a profile's boundary, lifted into world space.
struct WorldPiece {
    std::shared_ptr<const Curve3> curve;
    SketchId entity = kNoSketchId;
    double t0 = 0.0;
    double t1 = 1.0;
    Vec3d start;
    Vec3d end;
};

// Whether a loop is one closed curve that the arrangement merely cut up.
//
// It always does cut them up: a closed curve with nothing crossing it has
// only one endpoint and so cannot become a half-edge, so cad_arrangement
// splits it in half on purpose. For deciding topology that is right. For
// building a solid it is not, and the difference matters far more than it
// looks: a circle rebuilt as two half-faces has two seams where a
// cylinder has one, two surfaces where one would do, and two of every
// edge along them for the boolean to match up. Recognising the case and
// building the single seamed face that MakeCylinder builds turns an
// extruded circle into exactly the cylinder the rest of the kernel
// already knows how to cut.
bool IsOneClosedCurve(const std::vector<WorldPiece> &pieces) {
    if (pieces.empty()) return false;
    const SketchId entity = pieces.front().entity;
    double lo = 0.0;
    double hi = 0.0;
    pieces.front().curve->Domain(&lo, &hi);
    if ((pieces.front().curve->Point(lo) - pieces.front().curve->Point(hi)).Length() > 1e-9) return false;
    double covered = 0.0;
    for (const WorldPiece &piece : pieces) {
        if (piece.entity != entity) return false;
        covered += std::fabs(piece.t1 - piece.t0);
    }
    return std::fabs(covered - (hi - lo)) <= 1e-9 * std::max(1.0, hi - lo);
}

bool LiftLoop(const Sketch &sketch, const std::vector<Sketch::Profile::Piece> &pieces,
              std::vector<WorldPiece> *out, std::string *error) {
    out->clear();
    for (const Sketch::Profile::Piece &piece : pieces) {
        const std::shared_ptr<const Curve3> curve = sketch.WorldCurve(piece.entity);
        if (!curve) {
            *error = "sketch entity " + std::to_string(piece.entity) + " has no curve";
            return false;
        }
        WorldPiece lifted;
        lifted.curve = curve;
        lifted.entity = piece.entity;
        lifted.t0 = piece.t0;
        lifted.t1 = piece.t1;
        lifted.start = curve->Point(piece.t0);
        lifted.end = curve->Point(piece.t1);
        out->push_back(std::move(lifted));
    }
    if (out->size() < 2) {
        // Every loop the arrangement produces has at least two pieces,
        // because a closed curve with nothing to cross is cut in half
        // precisely so that it has two endpoints (see cad_arrangement.cpp).
        // One piece here would mean an edge with no vertices, which no
        // amount of care further down recovers from.
        *error = "a profile loop came back with fewer than two pieces";
        return false;
    }
    return true;
}

std::shared_ptr<Curve3> Moved(const Curve3 &curve, const Vec3d &offset) {
    std::shared_ptr<Curve3> copy(curve.Clone().release());
    copy->Transform(Mat4d::Translation(offset));
    return copy;
}

// The sketch-space bounding box of a profile, for sizing the cap planes.
Box3d ProfileExtent(const Sketch &sketch, const Sketch::Profile &profile) {
    Box3d box;
    auto add = [&](const std::vector<Sketch::Profile::Piece> &pieces) {
        for (const Sketch::Profile::Piece &piece : pieces) {
            const std::shared_ptr<const Curve3> flat = sketch.Curve(piece.entity);
            if (!flat) continue;
            for (int i = 0; i <= 16; ++i) {
                const double t = piece.t0 + (piece.t1 - piece.t0) * static_cast<double>(i) / 16.0;
                box.Expand(flat->Point(t));
            }
        }
    };
    add(profile.outer);
    for (const std::vector<Sketch::Profile::Piece> &hole : profile.holes) add(hole);
    return box;
}

// One swept loop's worth of topology: the vertices, edges and side faces
// a single boundary loop contributes.
struct SweptLoop {
    std::vector<EntityId> start_vertices;
    std::vector<EntityId> end_vertices;
    std::vector<EntityId> start_edges;  // the loop on the starting cap
    std::vector<EntityId> end_edges;    // the same loop on the far cap
    std::vector<EntityId> rail_edges;   // one per junction, joining the two
    std::vector<EntityId> side_faces;
};

// A loop traversed backwards: the coedges in reverse order, each flipped.
// Both caps use this on one side and not the other, which is what makes
// every edge come out used once each way.
std::vector<EntityId> ReversedCoEdges(Model *model, const std::vector<EntityId> &edges) {
    std::vector<EntityId> coedges;
    for (std::size_t i = edges.size(); i-- > 0;) {
        coedges.push_back(model->AddCoEdge(edges[i], Orientation::Reversed));
    }
    return coedges;
}

std::vector<EntityId> ForwardCoEdges(Model *model, const std::vector<EntityId> &edges) {
    std::vector<EntityId> coedges;
    for (EntityId edge : edges) coedges.push_back(model->AddCoEdge(edge, Orientation::Forward));
    return coedges;
}

// The surface a curve sweeps out along a straight direction -- as the
// simplest kind that can express it, not as a generic extrusion.
//
// This is canonical-surface recognition, and it is not an optimisation.
// A line swept is a plane and a circle swept along its own axis is a
// cylinder, and Part C's intersection code has closed-form answers for
// those pairs and a marching solver for everything else. Handing it an
// ExtrusionSurface wrapping a circle means marching around a surface that
// closes on itself, when the answer was a circle that could have been
// written down -- and a hole drilled through a plate came out
// non-manifold for exactly that reason, while the identical hole built
// from MakeCylinder came out clean.
std::shared_ptr<Surface> SweptSurface(const Curve3 &curve, double t0, double t1, const Vec3d &direction,
                                      double distance) {
    const Vec3d unit = direction.Normalized();
    const double lo = std::min(t0, t1);
    const double hi = std::max(t0, t1);

    if (curve.Kind() == CurveKind::Line) {
        const Vec3d a = curve.Point(lo);
        const Vec3d b = curve.Point(hi);
        const double length = (b - a).Length();
        if (length > 0.0) {
            return std::make_shared<PlaneSurface>(a, (b - a) / length, unit, 0.0, length, 0.0, distance);
        }
    }
    if (curve.Kind() == CurveKind::Circle) {
        const auto *circle = dynamic_cast<const Circle3 *>(&curve);
        if (circle != nullptr &&
            std::fabs(std::fabs(circle->PlaneNormal().Dot(unit)) - 1.0) < 1e-12) {
            // A circle swept along its own axis is a cylinder, and Part C
            // has a closed form for plane/cylinder where it would have to
            // march a generic extrusion. A hole drilled through a plate
            // came out non-manifold for want of this, while the identical
            // hole built from MakeCylinder came out clean.
            //
            // The cylinder keeps the *circle's* axis, not the sweep
            // direction, and its height range runs negative when the two
            // disagree. That is the whole trick. A cylinder's angle
            // increases counter-clockwise about its own axis, so taking
            // the sweep direction as the axis makes the angle run
            // backwards relative to the circle's parameter whenever they
            // oppose -- and Part B.2's seam placement then puts the
            // face's p-curves a full turn outside the surface's domain,
            // where nothing can find them. The face simply vanishes: the
            // solid still builds, and the boolean finds nothing to cut.
            // The cylinder's axis is the sweep direction, so its height
            // range is [0, distance] and the face's loop runs the usual
            // way round. Its angular parameter then runs backwards
            // relative to the circle's whenever the two axes oppose --
            // which is not hidden here but handed back to the caller,
            // because the loop's own direction has to follow it.
            const Vec3d axis = unit;
            const Vec3d radial = circle->Point(0.0) - circle->Center();
            const double radius = radial.Length();
            const Vec3d x_axis = radial / radius;
            const Vec3d y_axis = axis.Cross(x_axis);
            auto angle_at = [&](double t) {
                const Vec3d d = circle->Point(t) - circle->Center();
                return std::atan2(d.Dot(y_axis), d.Dot(x_axis));
            };
            auto unwrap = [](double value, double near) {
                while (value - near > kPi) value -= kTwoPi;
                while (near - value > kPi) value += kTwoPi;
                return value;
            };
            const double a0 = angle_at(lo);
            const double am = unwrap(angle_at(0.5 * (lo + hi)), a0);
            const double a1 = unwrap(angle_at(hi), am);
            double whole_lo = 0.0;
            double whole_hi = 0.0;
            curve.Domain(&whole_lo, &whole_hi);
            const bool whole_circle = std::fabs((hi - lo) - (whole_hi - whole_lo)) <=
                                      1e-9 * std::max(1.0, whole_hi - whole_lo);
            double u_lo = 0.0;
            double u_hi = 0.0;
            if (whole_circle) {
                // The face is the whole tube, so it has a seam, and a
                // seam belongs at the *edge* of the parameter range --
                // that is what makes it a seam rather than a cut through
                // the middle of the face.
                u_lo = (am > a0) ? a0 : a0 - kTwoPi;
                u_hi = u_lo + kTwoPi;
            } else {
                // A whole turn centred on the arc. Trimming to the arc
                // itself is worse than useless: ClosestPoint clamps to
                // the domain rather than wrapping, so a point on the far
                // side comes back sitting exactly on this face's own
                // boundary, and the point-in-face test then answers by
                // round-off at precisely the places an intersection curve
                // has its endpoints.
                const double middle = 0.5 * (a0 + a1);
                u_lo = middle - kPi;
                u_hi = middle + kPi;
            }
            // Not padded along the axis. A cylinder's own seam already
            // sits at the edge of its angular range, and moving its ends
            // away from the face's rims costs the seam placement in Part
            // B.2 the very landmarks it uses -- a hole cut through a
            // plate came back with one rim edge used only once.
            return std::make_shared<CylinderSurface>(circle->Center(), x_axis, axis, radius, u_lo, u_hi,
                                                     0.0, distance);
        }
    }
    std::unique_ptr<Curve3> profile = curve.Clone();
    return std::make_shared<ExtrusionSurface>(std::move(profile), unit, 0.0, distance);
}

}  // namespace

bool SelectProfile(const Sketch &sketch, int profile_index, Sketch::Profile *out, std::string *error) {
    std::vector<Sketch::Profile> profiles;
    std::string extract_error;
    if (!sketch.ExtractProfiles(&profiles, &extract_error)) {
        *error = extract_error.empty() ? "the sketch's geometry could not be arranged" : extract_error;
        return false;
    }
    if (profiles.empty()) {
        *error = "the sketch has no closed region to build from";
        return false;
    }
    if (profile_index >= 0) {
        if (static_cast<std::size_t>(profile_index) >= profiles.size()) {
            *error = "the sketch has only " + std::to_string(profiles.size()) + " profile(s)";
            return false;
        }
        *out = profiles[static_cast<std::size_t>(profile_index)];
        return true;
    }
    // The largest. A closed outline with holes inside it comes out as the
    // outline-with-holes *and* each hole's own disc, and the outline is
    // the one meant -- asking for "the profile" of a plate with two holes
    // should not sometimes give you one of the holes.
    std::size_t best = 0;
    for (std::size_t i = 1; i < profiles.size(); ++i) {
        if (profiles[i].area > profiles[best].area) best = i;
    }
    *out = profiles[best];
    return true;
}

// --- Extrude -----------------------------------------------------------

bool ExtrudeProfile(const Sketch &sketch, const Sketch::Profile &profile, const Vec3d &direction,
                    double distance, Model *out, EntityId *out_body, std::string *error) {
    error->clear();
    if (!(std::fabs(distance) > 0.0)) {
        *error = "an extrude needs a non-zero distance";
        return false;
    }
    const Vec3d offset = direction.Normalized() * distance;
    const SketchPlane &plane = sketch.Plane();
    if (std::fabs(offset.Dot(plane.Normal())) <= 1e-12) {
        *error = "the extrude direction lies in the sketch plane";
        return false;
    }
    // Extruding against the plane's own normal swaps which cap is the
    // near one, and with it every orientation below. Rather than carry a
    // sign through the whole construction, the profile is built from the
    // far end -- the geometry is the same solid either way.
    const bool along_normal = offset.Dot(plane.Normal()) > 0.0;

    std::vector<std::vector<Sketch::Profile::Piece>> loops;
    loops.push_back(profile.outer);
    for (const std::vector<Sketch::Profile::Piece> &hole : profile.holes) loops.push_back(hole);

    std::vector<SweptLoop> swept;
    for (const std::vector<Sketch::Profile::Piece> &loop : loops) {
        std::vector<WorldPiece> pieces;
        if (!LiftLoop(sketch, loop, &pieces, error)) return false;
        const std::size_t n = pieces.size();

        SweptLoop built;
        if (IsOneClosedCurve(pieces)) {
            // One face with a seam, the way MakeCylinder builds a tube.
            const Curve3 &curve = *pieces.front().curve;
            double lo = 0.0;
            double hi = 0.0;
            curve.Domain(&lo, &hi);
            // Which way round the loop actually goes. A hole's loop runs
            // clockwise, and the curve's own parameter runs
            // counter-clockwise, so a closed hole built from the curve's
            // direction comes out as an outer loop with the wrong sign --
            // which the validator catches as a hole enclosing positive
            // area, and which would otherwise have added the hole's
            // volume instead of removing it.
            double signed_span = 0.0;
            for (const WorldPiece &piece : pieces) signed_span += piece.t1 - piece.t0;
            const bool ccw = signed_span >= 0.0;
            const double from = ccw ? lo : hi;
            const double to = ccw ? hi : lo;
            const Vec3d seam = curve.Point(from);
            const EntityId near_seam = out->AddVertex(seam);
            const EntityId far_seam = out->AddVertex(seam + offset);
            const int near_curve = out->AddCurve(std::shared_ptr<const Curve3>(curve.Clone().release()));
            const int far_curve = out->AddCurve(Moved(curve, offset));
            const int rail_curve =
                out->AddCurve(std::make_shared<Line3>(Line3::FromPoints(seam, seam + offset)));
            // Closed edges: they start and end at the same vertex, having
            // gone all the way round.
            built.start_vertices = {near_seam};
            built.end_vertices = {far_seam};
            built.start_edges = {out->AddEdge(near_curve, near_seam, near_seam, from, to)};
            built.end_edges = {out->AddEdge(far_curve, far_seam, far_seam, from, to)};
            built.rail_edges = {out->AddEdge(rail_curve, near_seam, far_seam, 0.0, 1.0)};

            const int surface =
                out->AddSurface(SweptSurface(curve, lo, hi, offset.Normalized(), offset.Length()));

            // The same loop MakeCylinder walks -- along the near rim, up
            // the seam, back along the far rim, down the seam -- but
            // which way "along the near rim" is takes working out.
            //
            // The loop has to run counter-clockwise in the surface's own
            // (u, v), and v increases along the sweep, so the near rim
            // must be traversed in *increasing u*. On a cylinder whose
            // axis is the sweep direction, u increases counter-clockwise
            // about that direction -- which is the circle's own direction
            // only when the sweep goes along the circle's normal. So a
            // clockwise loop, or a sweep against the normal, reverses it,
            // and both together reverse it back.
            //
            // Getting this wrong does not produce a wrong solid; it
            // produces a face whose p-curves land a full turn outside its
            // surface's domain, where nothing can find them. The face
            // vanishes, the body still validates, and a boolean against
            // it silently finds nothing to cut.
            const bool u_increases = (ccw == along_normal);
            std::vector<EntityId> coedges;
            coedges.push_back(out->AddCoEdge(built.start_edges[0],
                                             u_increases ? Orientation::Forward : Orientation::Reversed));
            coedges.push_back(out->AddCoEdge(built.rail_edges[0], Orientation::Forward));
            coedges.push_back(out->AddCoEdge(built.end_edges[0],
                                             u_increases ? Orientation::Reversed : Orientation::Forward));
            coedges.push_back(out->AddCoEdge(built.rail_edges[0], Orientation::Reversed));
            const EntityId loop_id = out->AddLoop(coedges, true);
            // The surface normal is (direction of increasing u) x (sweep),
            // and the outward direction is (direction the loop runs) x
            // (the plane's normal). Working the two signs through leaves
            // just this: the surface's own normal is the outward one
            // exactly when the loop runs counter-clockwise in the sketch.
            built.side_faces.push_back(out->AddFace(
                surface, ccw ? Orientation::Forward : Orientation::Reversed, {loop_id},
                "side " + std::to_string(swept.size()) + ".0"));
            swept.push_back(std::move(built));
            continue;
        }
        for (const WorldPiece &piece : pieces) {
            built.start_vertices.push_back(out->AddVertex(piece.start));
            built.end_vertices.push_back(out->AddVertex(piece.start + offset));
        }
        for (std::size_t i = 0; i < n; ++i) {
            const WorldPiece &piece = pieces[i];
            const std::size_t next = (i + 1) % n;
            const int near_curve = out->AddCurve(std::shared_ptr<const Curve3>(piece.curve->Clone().release()));
            const int far_curve = out->AddCurve(Moved(*piece.curve, offset));
            built.start_edges.push_back(out->AddEdge(near_curve, built.start_vertices[i],
                                                     built.start_vertices[next], piece.t0, piece.t1));
            built.end_edges.push_back(
                out->AddEdge(far_curve, built.end_vertices[i], built.end_vertices[next], piece.t0, piece.t1));
            const int rail_curve = out->AddCurve(
                std::make_shared<Line3>(Line3::FromPoints(piece.start, piece.start + offset)));
            built.rail_edges.push_back(
                out->AddEdge(rail_curve, built.start_vertices[i], built.end_vertices[i], 0.0, 1.0));
        }

        // The side faces.
        //
        // Each is a piece of the boundary swept along `offset`, and its
        // surface normal is T x offset where T is the boundary's tangent.
        // That is outward for *both* an outer loop and a hole, which is
        // worth stating because it looks as though it should not be: an
        // outer loop runs counter-clockwise and a hole clockwise, so the
        // tangents run opposite ways -- but the material is on the left
        // in one case and on the right in the other, and the two
        // reversals cancel. So there is no special case here, and a
        // construction that introduced one would be wrong.
        for (std::size_t i = 0; i < n; ++i) {
            const WorldPiece &piece = pieces[i];
            const std::size_t next = (i + 1) % n;
            const int surface = out->AddSurface(
                SweptSurface(*piece.curve, piece.t0, piece.t1, offset.Normalized(), offset.Length()));

            // The loop, counter-clockwise in the surface's own (u, v):
            // along the near edge in increasing u, up the rail at the
            // larger u, back along the far edge, down the rail at the
            // smaller u. A piece whose parameters descend -- which the
            // arrangement produces whenever it reversed a half-edge --
            // has its two rails the other way round, and nothing else
            // about it changes.
            const bool ascending = piece.t1 >= piece.t0;
            const EntityId rail_at_min = ascending ? built.rail_edges[i] : built.rail_edges[next];
            const EntityId rail_at_max = ascending ? built.rail_edges[next] : built.rail_edges[i];
            std::vector<EntityId> coedges;
            coedges.push_back(out->AddCoEdge(built.start_edges[i],
                                             ascending ? Orientation::Forward : Orientation::Reversed));
            coedges.push_back(out->AddCoEdge(rail_at_max, Orientation::Forward));
            coedges.push_back(out->AddCoEdge(built.end_edges[i],
                                             ascending ? Orientation::Reversed : Orientation::Forward));
            coedges.push_back(out->AddCoEdge(rail_at_min, Orientation::Reversed));
            const EntityId loop_id = out->AddLoop(coedges, true);
            // Whether the surface's own normal is the outward one.
            //
            // The surface normal is dC/du x dir. The outward direction is
            // T x n, where T is the tangent *the loop* runs in and n is
            // the sketch plane's normal -- and that is outward for an
            // outer loop and a hole alike, because the material is on the
            // left in one and on the right in the other and the two
            // reversals cancel. Two things can put the surface's normal
            // the other way round. A piece whose parameters descend has
            // dC/du = -T. And an extrude against the plane's normal has
            // dir = -n. Either one flips it; both together flip it back.
            const bool outward = (ascending == along_normal);
            // Each side face is named for the loop and the piece it came
            // from, not just "side". A face's role is one of the
            // strongest signals Part B.4 has, and giving every side of a
            // block the same one throws it away -- which leaves a
            // reference to one of them ambiguous against all the others,
            // and a fillet unable to find its own edge after a rebuild.
            built.side_faces.push_back(
                out->AddFace(surface, outward ? Orientation::Forward : Orientation::Reversed, {loop_id},
                             "side " + std::to_string(swept.size()) + "." + std::to_string(i)));
        }
        swept.push_back(std::move(built));
    }

    // The caps.
    //
    // The near one's outward normal points away from the extrude, so its
    // plane is built with its axes swapped -- u along the sketch's y and
    // v along its x -- which negates the normal without having to flip
    // the face. Its loops are then traversed backwards, and the far cap's
    // forwards, which is what leaves every edge used once each way.
    const Box3d extent = ProfileExtent(sketch, profile);
    const double pad = std::max(1.0, extent.Extent().Length()) * 0.25;
    const Vec3d near_origin = along_normal ? plane.origin : plane.origin + offset;
    const Vec3d far_origin = along_normal ? plane.origin + offset : plane.origin;

    const int near_surface = out->AddSurface(std::make_shared<PlaneSurface>(
        near_origin, plane.y_axis, plane.x_axis, extent.y.lo - pad, extent.y.hi + pad, extent.x.lo - pad,
        extent.x.hi + pad));
    const int far_surface = out->AddSurface(std::make_shared<PlaneSurface>(
        far_origin, plane.x_axis, plane.y_axis, extent.x.lo - pad, extent.x.hi + pad, extent.y.lo - pad,
        extent.y.hi + pad));

    std::vector<EntityId> near_loops;
    std::vector<EntityId> far_loops;
    for (std::size_t i = 0; i < swept.size(); ++i) {
        const SweptLoop &built = swept[i];
        const std::vector<EntityId> &near_edges = along_normal ? built.start_edges : built.end_edges;
        const std::vector<EntityId> &far_edges = along_normal ? built.end_edges : built.start_edges;
        near_loops.push_back(out->AddLoop(ReversedCoEdges(out, near_edges), i == 0));
        far_loops.push_back(out->AddLoop(ForwardCoEdges(out, far_edges), i == 0));
    }
    const EntityId near_face = out->AddFace(near_surface, Orientation::Forward, near_loops, "start");
    const EntityId far_face = out->AddFace(far_surface, Orientation::Forward, far_loops, "end");

    std::vector<EntityId> faces{near_face, far_face};
    for (const SweptLoop &built : swept) {
        faces.insert(faces.end(), built.side_faces.begin(), built.side_faces.end());
    }
    const EntityId shell = out->AddShell(faces, true);
    *out_body = out->AddBody({shell}, "extrude");

    std::string pcurve_error;
    if (!BuildAllPCurves(out, {}, &pcurve_error)) {
        *error = "could not build p-curves on the extrusion: " + pcurve_error;
        return false;
    }
    return true;
}

// --- Revolve -----------------------------------------------------------

bool RevolveProfile(const Sketch &sketch, const Sketch::Profile &profile, const Vec2d &axis_point,
                    const Vec2d &axis_direction, double angle, Model *out, EntityId *out_body,
                    std::string *error) {
    error->clear();
    if (!(std::fabs(angle) > 1e-9)) {
        *error = "a revolve needs a non-zero angle";
        return false;
    }
    if (axis_direction.Length() <= 0.0) {
        *error = "the revolve axis has no direction";
        return false;
    }
    const bool full_turn = std::fabs(std::fabs(angle) - kTwoPi) < 1e-9;
    const SketchPlane &plane = sketch.Plane();
    const Vec3d axis_origin = plane.ToWorld(axis_point);
    const Vec2d unit = axis_direction / axis_direction.Length();
    const Vec3d axis = (plane.x_axis * unit.x + plane.y_axis * unit.y).Normalized();

    // Everything must be on one side of the axis, and none of it may
    // touch. A profile crossing its own axis of revolution sweeps through
    // itself, and the result is not a solid -- it is a self-intersecting
    // surface that every later operation would then have to cope with.
    // Refusing here is the only place the check is cheap.
    double profile_side = 0.0;
    {
        double lowest = 0.0;
        double highest = 0.0;
        bool first = true;
        auto check = [&](const std::vector<Sketch::Profile::Piece> &pieces) {
            for (const Sketch::Profile::Piece &piece : pieces) {
                const std::shared_ptr<const Curve3> flat = sketch.Curve(piece.entity);
                if (!flat) continue;
                for (int i = 0; i <= 24; ++i) {
                    const double t = piece.t0 + (piece.t1 - piece.t0) * static_cast<double>(i) / 24.0;
                    const Vec3d p = flat->Point(t);
                    const Vec2d d{p.x - axis_point.x, p.y - axis_point.y};
                    const double side = d.Cross(unit);
                    lowest = first ? side : std::min(lowest, side);
                    highest = first ? side : std::max(highest, side);
                    first = false;
                }
            }
        };
        check(profile.outer);
        for (const std::vector<Sketch::Profile::Piece> &hole : profile.holes) check(hole);
        const double span = std::max(std::fabs(lowest), std::fabs(highest));
        if (lowest < -1e-9 * std::max(1.0, span) && highest > 1e-9 * std::max(1.0, span)) {
            *error = "the profile crosses its own axis of revolution";
            return false;
        }
        profile_side = 0.5 * (lowest + highest);
    }

    // Which way the sweep sets off, relative to the sketch plane's normal.
    //
    // An extrude sweeps along a direction the caller gave, so comparing
    // it with the plane's normal is a dot product. A revolve's sweep
    // direction is axis x radius, which depends on which side of the axis
    // the profile sits -- a profile to the left of the axis sets off one
    // way and a mirror image of it the other, for the same axis and the
    // same angle. Getting this from the geometry rather than assuming it
    // is what keeps a revolve from coming out inside out, which it does
    // *validly*: every face is sewn correctly to its neighbours and the
    // volume is negative.
    const bool sweep_along_normal = (profile_side < 0.0) == (angle > 0.0);

    std::vector<std::vector<Sketch::Profile::Piece>> loops;
    loops.push_back(profile.outer);
    for (const std::vector<Sketch::Profile::Piece> &hole : profile.holes) loops.push_back(hole);

    auto rotate = [&](const Vec3d &p, double by) {
        const Mat4d m = Mat4d::Translation(axis_origin) * Mat4d::Rotation(axis, by) *
                        Mat4d::Translation(-axis_origin);
        return m.TransformPoint(p);
    };

    std::vector<SweptLoop> swept;
    for (const std::vector<Sketch::Profile::Piece> &loop : loops) {
        std::vector<WorldPiece> pieces;
        if (!LiftLoop(sketch, loop, &pieces, error)) return false;
        const std::size_t n = pieces.size();
        SweptLoop built;
        for (const WorldPiece &piece : pieces) {
            built.start_vertices.push_back(out->AddVertex(piece.start));
            built.end_vertices.push_back(full_turn ? built.start_vertices.back()
                                                   : out->AddVertex(rotate(piece.start, angle)));
        }
        const Mat4d turn = Mat4d::Translation(axis_origin) * Mat4d::Rotation(axis, angle) *
                           Mat4d::Translation(-axis_origin);
        for (std::size_t i = 0; i < n; ++i) {
            const WorldPiece &piece = pieces[i];
            const std::size_t next = (i + 1) % n;
            const int near_curve = out->AddCurve(std::shared_ptr<const Curve3>(piece.curve->Clone().release()));
            built.start_edges.push_back(out->AddEdge(near_curve, built.start_vertices[i],
                                                     built.start_vertices[next], piece.t0, piece.t1));
            if (full_turn) {
                // The two ends of the sweep are the same place, so the
                // profile's own edge serves as both -- which is exactly
                // what a seam is.
                built.end_edges.push_back(built.start_edges.back());
            } else {
                std::shared_ptr<Curve3> turned(piece.curve->Clone().release());
                turned->Transform(turn);
                const int far_curve = out->AddCurve(turned);
                built.end_edges.push_back(out->AddEdge(far_curve, built.end_vertices[i],
                                                       built.end_vertices[next], piece.t0, piece.t1));
            }
            // The rail: the circular path the junction point traces.
            const Vec3d from_axis = piece.start - axis_origin;
            const double along = from_axis.Dot(axis);
            const Vec3d radial = from_axis - axis * along;
            const double radius = radial.Length();
            if (radius <= 1e-12) {
                // The junction is on the axis: it does not move, so there
                // is no rail edge, only a point. That is a degeneracy the
                // surfaces cope with (it is a pole) but the topology here
                // does not, so it is refused rather than mis-built.
                *error = "the profile touches its axis of revolution";
                return false;
            }
            const Vec3d rail_x = radial / radius;
            const Vec3d rail_y = axis.Cross(rail_x);
            const int rail_curve = out->AddCurve(std::make_shared<Circle3>(
                axis_origin + axis * along, rail_x, rail_y, radius, 0.0, full_turn ? kTwoPi : angle));
            built.rail_edges.push_back(out->AddEdge(rail_curve, built.start_vertices[i],
                                                    built.end_vertices[i], 0.0,
                                                    full_turn ? kTwoPi : angle));
        }

        for (std::size_t i = 0; i < n; ++i) {
            const WorldPiece &piece = pieces[i];
            const std::size_t next = (i + 1) % n;
            std::unique_ptr<Curve3> rail_profile = piece.curve->Clone();
            const int surface = out->AddSurface(std::make_shared<RevolutionSurface>(
                std::move(rail_profile), axis_origin, axis, 0.0, full_turn ? kTwoPi : angle));
            const bool ascending = piece.t1 >= piece.t0;
            const EntityId rail_at_min = ascending ? built.rail_edges[i] : built.rail_edges[next];
            const EntityId rail_at_max = ascending ? built.rail_edges[next] : built.rail_edges[i];
            std::vector<EntityId> coedges;
            coedges.push_back(out->AddCoEdge(built.start_edges[i],
                                             ascending ? Orientation::Forward : Orientation::Reversed));
            coedges.push_back(out->AddCoEdge(rail_at_max, Orientation::Forward));
            coedges.push_back(out->AddCoEdge(built.end_edges[i],
                                             ascending ? Orientation::Reversed : Orientation::Forward));
            coedges.push_back(out->AddCoEdge(rail_at_min, Orientation::Reversed));
            const EntityId loop_id = out->AddLoop(coedges, true);
            // Exactly the extrude's argument, with the sweep direction
            // worked out above instead of given.
            const bool outward = (ascending == sweep_along_normal);
            built.side_faces.push_back(out->AddFace(
                surface, outward ? Orientation::Forward : Orientation::Reversed, {loop_id}, "side"));
        }
        swept.push_back(std::move(built));
    }

    std::vector<EntityId> faces;
    for (const SweptLoop &built : swept) {
        faces.insert(faces.end(), built.side_faces.begin(), built.side_faces.end());
    }
    if (!full_turn) {
        // The two flat ends, each the profile in its own plane. The
        // starting one faces backwards along the sweep and so has its
        // axes swapped, exactly as an extrude's near cap does.
        const Box3d extent = ProfileExtent(sketch, profile);
        const double pad = std::max(1.0, extent.Extent().Length()) * 0.25;
        const Vec3d swept_x = rotate(plane.origin + plane.x_axis, angle) - rotate(plane.origin, angle);
        const Vec3d swept_y = rotate(plane.origin + plane.y_axis, angle) - rotate(plane.origin, angle);
        const Vec3d swept_origin = rotate(plane.origin, angle);
        // The starting cap faces backwards along the sweep and the ending
        // one forwards, so exactly one of them has its plane's axes
        // swapped -- which negates that plane's normal -- and exactly one
        // has its loops traversed backwards. Which one is which follows
        // from the sweep direction, the same as everything else here.
        auto cap = [&](const Vec3d &origin, const Vec3d &ax, const Vec3d &ay, const Interval &ex,
                       const Interval &ey, bool flip, const std::vector<EntityId> SweptLoop::*edges,
                       const char *name) {
            const int surface = out->AddSurface(std::make_shared<PlaneSurface>(
                origin, flip ? ay : ax, flip ? ax : ay, (flip ? ey.lo : ex.lo) - pad,
                (flip ? ey.hi : ex.hi) + pad, (flip ? ex.lo : ey.lo) - pad, (flip ? ex.hi : ey.hi) + pad));
            std::vector<EntityId> loops;
            for (std::size_t i = 0; i < swept.size(); ++i) {
                loops.push_back(out->AddLoop(flip ? ReversedCoEdges(out, swept[i].*edges)
                                                  : ForwardCoEdges(out, swept[i].*edges),
                                             i == 0));
            }
            return out->AddFace(surface, Orientation::Forward, loops, name);
        };
        faces.push_back(cap(plane.origin, plane.x_axis, plane.y_axis, extent.x, extent.y,
                            sweep_along_normal, &SweptLoop::start_edges, "start"));
        faces.push_back(cap(swept_origin, swept_x, swept_y, extent.x, extent.y, !sweep_along_normal,
                            &SweptLoop::end_edges, "end"));
    }

    const EntityId shell = out->AddShell(faces, true);
    *out_body = out->AddBody({shell}, "revolve");

    std::string pcurve_error;
    if (!BuildAllPCurves(out, {}, &pcurve_error)) {
        *error = "could not build p-curves on the revolution: " + pcurve_error;
        return false;
    }
    return true;
}

// --- Loft and sweep ----------------------------------------------------
//
// Both build the same thing: a stack of closed sections, corresponding
// pieces joined by a ruled surface, and a flat cap at each end. They
// differ only in where the sections come from -- given, for a loft;
// generated along a spine, for a sweep -- so the construction is written
// once and the two features supply rings to it.

namespace {

// One section of a swept or lofted solid: its loops in world space, and
// the plane they lie in so the end caps can be built.
struct Ring {
    std::vector<std::vector<WorldPiece>> loops;
    Vec3d origin;
    Vec3d x_axis;
    Vec3d y_axis;
    Interval u;
    Interval v;
};

bool BuildRuledSolid(const std::vector<Ring> &rings, const char *name, Model *out, EntityId *out_body,
                     std::string *error) {
    if (rings.size() < 2) {
        *error = "a lofted or swept solid needs at least two sections";
        return false;
    }
    const std::size_t levels = rings.size();
    const std::size_t loop_count = rings.front().loops.size();
    for (std::size_t s = 0; s < levels; ++s) {
        if (rings[s].loops.size() != loop_count) {
            *error = "every section must have the same number of loops";
            return false;
        }
        for (std::size_t l = 0; l < loop_count; ++l) {
            // Every section must have the same number of boundary pieces
            // in each loop, because one ruled surface runs between each
            // corresponding pair. Matching sections that do not
            // correspond -- a triangle to a circle -- is a real feature
            // and a much larger one: it needs the sections
            // re-parameterised against each other first, and guessing a
            // correspondence is how a loft produces a twisted,
            // self-intersecting mess that still validates.
            if (rings[s].loops[l].size() != rings.front().loops[l].size()) {
                *error = "every section must have the same number of boundary pieces (" +
                         std::to_string(rings.front().loops[l].size()) + " here, " +
                         std::to_string(rings[s].loops[l].size()) + " in section " + std::to_string(s) + ")";
                return false;
            }
        }
    }

    // Vertices and ring edges, level by level.
    std::vector<std::vector<std::vector<EntityId>>> vertices(levels);
    std::vector<std::vector<std::vector<EntityId>>> ring_edges(levels);
    for (std::size_t s = 0; s < levels; ++s) {
        vertices[s].resize(loop_count);
        ring_edges[s].resize(loop_count);
        for (std::size_t l = 0; l < loop_count; ++l) {
            const std::vector<WorldPiece> &pieces = rings[s].loops[l];
            for (const WorldPiece &piece : pieces) vertices[s][l].push_back(out->AddVertex(piece.start));
            for (std::size_t i = 0; i < pieces.size(); ++i) {
                const int curve =
                    out->AddCurve(std::shared_ptr<const Curve3>(pieces[i].curve->Clone().release()));
                ring_edges[s][l].push_back(out->AddEdge(curve, vertices[s][l][i],
                                                        vertices[s][l][(i + 1) % pieces.size()],
                                                        pieces[i].t0, pieces[i].t1));
            }
        }
    }

    std::vector<EntityId> faces;
    for (std::size_t s = 0; s + 1 < levels; ++s) {
        for (std::size_t l = 0; l < loop_count; ++l) {
            const std::vector<WorldPiece> &pieces = rings[s].loops[l];
            const std::size_t n = pieces.size();
            std::vector<EntityId> rails;
            for (std::size_t i = 0; i < n; ++i) {
                const int curve = out->AddCurve(std::make_shared<Line3>(
                    Line3::FromPoints(pieces[i].start, rings[s + 1].loops[l][i].start)));
                rails.push_back(out->AddEdge(curve, vertices[s][l][i], vertices[s + 1][l][i], 0.0, 1.0));
            }
            for (std::size_t i = 0; i < n; ++i) {
                const std::size_t next = (i + 1) % n;
                // A ruled surface between the two corresponding pieces.
                // Its v runs from this section to the next and its u
                // along the pieces, so the extrude's orientation argument
                // carries over unchanged.
                std::unique_ptr<Curve3> a = pieces[i].curve->Clone();
                std::unique_ptr<Curve3> b = rings[s + 1].loops[l][i].curve->Clone();
                const int surface =
                    out->AddSurface(std::make_shared<RuledSurface>(std::move(a), std::move(b)));
                const bool ascending = pieces[i].t1 >= pieces[i].t0;
                const EntityId rail_at_min = ascending ? rails[i] : rails[next];
                const EntityId rail_at_max = ascending ? rails[next] : rails[i];
                std::vector<EntityId> coedges;
                coedges.push_back(out->AddCoEdge(ring_edges[s][l][i],
                                                 ascending ? Orientation::Forward : Orientation::Reversed));
                coedges.push_back(out->AddCoEdge(rail_at_max, Orientation::Forward));
                coedges.push_back(out->AddCoEdge(ring_edges[s + 1][l][i],
                                                 ascending ? Orientation::Reversed : Orientation::Forward));
                coedges.push_back(out->AddCoEdge(rail_at_min, Orientation::Reversed));
                const EntityId loop_id = out->AddLoop(coedges, true);
                faces.push_back(out->AddFace(surface, ascending ? Orientation::Forward : Orientation::Reversed,
                                             {loop_id}, "side"));
            }
        }
    }

    // The caps, as for an extrude: the near one's axes swapped so its
    // normal points away from the sweep, and its loops traversed
    // backwards, which is what leaves every edge used once each way.
    auto cap = [&](std::size_t level, bool near_end) {
        const Ring &ring = rings[level];
        const double pad =
            std::max(1.0, std::max(ring.u.Width(), ring.v.Width())) * 0.25;
        const int surface = out->AddSurface(std::make_shared<PlaneSurface>(
            ring.origin, near_end ? ring.y_axis : ring.x_axis, near_end ? ring.x_axis : ring.y_axis,
            (near_end ? ring.v.lo : ring.u.lo) - pad, (near_end ? ring.v.hi : ring.u.hi) + pad,
            (near_end ? ring.u.lo : ring.v.lo) - pad, (near_end ? ring.u.hi : ring.v.hi) + pad));
        std::vector<EntityId> loops;
        for (std::size_t l = 0; l < loop_count; ++l) {
            loops.push_back(out->AddLoop(near_end ? ReversedCoEdges(out, ring_edges[level][l])
                                                  : ForwardCoEdges(out, ring_edges[level][l]),
                                         l == 0));
        }
        return out->AddFace(surface, Orientation::Forward, loops, near_end ? "start" : "end");
    };
    faces.push_back(cap(0, true));
    faces.push_back(cap(levels - 1, false));

    const EntityId shell = out->AddShell(faces, true);
    *out_body = out->AddBody({shell}, name);

    std::string pcurve_error;
    if (!BuildAllPCurves(out, {}, &pcurve_error)) {
        *error = std::string("could not build p-curves on the ") + name + ": " + pcurve_error;
        return false;
    }
    return true;
}

// The plane a sketch profile lies in, and the profile's extent within it.
void RingPlaneFromSketch(const Sketch &sketch, const Sketch::Profile &profile, Ring *ring) {
    const Box3d extent = ProfileExtent(sketch, profile);
    ring->origin = sketch.Plane().origin;
    ring->x_axis = sketch.Plane().x_axis;
    ring->y_axis = sketch.Plane().y_axis;
    ring->u = extent.x;
    ring->v = extent.y;
}

}  // namespace

bool LoftProfiles(const std::vector<const Sketch *> &sketches, const std::vector<Sketch::Profile> &profiles,
                  Model *out, EntityId *out_body, std::string *error) {
    error->clear();
    if (sketches.size() != profiles.size() || sketches.size() < 2) {
        *error = "a loft needs at least two sections";
        return false;
    }
    std::vector<Ring> rings;
    for (std::size_t s = 0; s < sketches.size(); ++s) {
        Ring ring;
        std::vector<WorldPiece> outer;
        if (!LiftLoop(*sketches[s], profiles[s].outer, &outer, error)) return false;
        ring.loops.push_back(std::move(outer));
        for (const std::vector<Sketch::Profile::Piece> &hole : profiles[s].holes) {
            std::vector<WorldPiece> pieces;
            if (!LiftLoop(*sketches[s], hole, &pieces, error)) return false;
            ring.loops.push_back(std::move(pieces));
        }
        RingPlaneFromSketch(*sketches[s], profiles[s], &ring);
        rings.push_back(std::move(ring));
    }
    return BuildRuledSolid(rings, "loft", out, out_body, error);
}

bool SweepProfile(const Sketch &sketch, const Sketch::Profile &profile, const Curve3 &spine, int sections,
                  double twist, double end_scale, Model *out, EntityId *out_body, std::string *error) {
    error->clear();
    if (sections < 2) {
        *error = "a sweep needs at least two sections";
        return false;
    }
    if (!(end_scale > 0.0)) {
        *error = "a sweep's end scale must be positive";
        return false;
    }
    double lo = 0.0;
    double hi = 0.0;
    spine.Domain(&lo, &hi);

    // Rotation-minimizing frames, not Frenet.
    //
    // The Frenet frame is undefined wherever the spine is momentarily
    // straight and spins through 180 degrees at an inflection, so a
    // Frenet sweep along an S-curve twists the profile visibly and a
    // sweep along a straight run has no frame at all. Part A.7 computes
    // the rotation-minimizing frame by double reflection, which is
    // defined everywhere the tangent is and carries no twist of its own
    // -- leaving whatever twist the feature asks for, and no more.
    std::vector<double> parameters;
    for (int i = 0; i < sections; ++i) {
        parameters.push_back(lo + (hi - lo) * static_cast<double>(i) / static_cast<double>(sections - 1));
    }
    const std::vector<Frame> frames = RotationMinimizingFrames(spine, parameters, sketch.Plane().x_axis);
    if (frames.size() != parameters.size()) {
        *error = "could not build frames along the spine";
        return false;
    }

    // The profile's own frame, which each section is mapped out of.
    Mat4d from_sketch;
    if (!Mat4d::Frame(sketch.Plane().origin, sketch.Plane().Normal(), sketch.Plane().x_axis)
             .Inverse(&from_sketch)) {
        *error = "the sketch plane is degenerate";
        return false;
    }

    std::vector<Ring> rings;
    for (std::size_t i = 0; i < frames.size(); ++i) {
        const double along = static_cast<double>(i) / static_cast<double>(frames.size() - 1);
        const double scale = 1.0 + (end_scale - 1.0) * along;
        const Mat4d place = Mat4d::Frame(frames[i].origin, frames[i].tangent, frames[i].normal) *
                            Mat4d::Rotation(Vec3d{0.0, 0.0, 1.0}, twist * along) *
                            Mat4d::Scaling(Vec3d{scale, scale, 1.0}) * from_sketch;

        Ring ring;
        auto place_loop = [&](const std::vector<Sketch::Profile::Piece> &pieces) {
            std::vector<WorldPiece> lifted;
            if (!LiftLoop(sketch, pieces, &lifted, error)) return false;
            for (WorldPiece &piece : lifted) {
                std::shared_ptr<Curve3> moved(piece.curve->Clone().release());
                moved->Transform(place);
                piece.curve = moved;
                piece.start = moved->Point(piece.t0);
                piece.end = moved->Point(piece.t1);
            }
            ring.loops.push_back(std::move(lifted));
            return true;
        };
        if (!place_loop(profile.outer)) return false;
        for (const std::vector<Sketch::Profile::Piece> &hole : profile.holes) {
            if (!place_loop(hole)) return false;
        }
        const Box3d extent = ProfileExtent(sketch, profile);
        ring.origin = frames[i].origin;
        ring.x_axis = frames[i].normal;
        ring.y_axis = frames[i].binormal;
        ring.u = Interval{extent.x.lo * scale, extent.x.hi * scale};
        ring.v = Interval{extent.y.lo * scale, extent.y.hi * scale};
        rings.push_back(std::move(ring));
    }
    return BuildRuledSolid(rings, "sweep", out, out_body, error);
}


// --- Naming an edge by the faces it lies between -----------------------
//
// Part B.4 will not guess: it reports a reference it cannot pin down as
// ambiguous rather than picking one, deliberately, because resolving
// wrongly costs a silently incorrect part. A box's twelve edges are
// alike in everything it scores on, so a direct reference to one of them
// is ambiguous against the rest and always will be. Its two faces are
// not alike -- different normals, different roles, different neighbours
// -- so naming them instead is both stronger and the usual practice.

bool CaptureEdgeBetweenFaces(const Model &model, EntityId edge, int generating_feature, EntityName *face_a,
                             EntityName *face_b) {
    std::vector<EntityId> faces;
    for (const Face &face : model.Faces()) {
        for (EntityId loop_id : face.loops) {
            const Loop *loop = model.GetLoop(loop_id);
            if (loop == nullptr) continue;
            for (EntityId coedge_id : loop->coedges) {
                const CoEdge *coedge = model.GetCoEdge(coedge_id);
                if (coedge != nullptr && coedge->edge == edge &&
                    std::find(faces.begin(), faces.end(), face.id) == faces.end()) {
                    faces.push_back(face.id);
                }
            }
        }
    }
    if (faces.size() != 2) return false;
    *face_a = CaptureFaceName(model, faces[0], generating_feature, model.GetFace(faces[0])->name);
    *face_b = CaptureFaceName(model, faces[1], generating_feature, model.GetFace(faces[1])->name);
    return true;
}

bool ResolveEdgeBetweenFaces(const Model &model, EntityId body, const EntityName &face_a,
                             const EntityName &face_b, EntityId *out, std::string *error) {
    const ResolveResult a = ResolveFace(model, face_a, {});
    const ResolveResult b = ResolveFace(model, face_b, {});
    auto describe = [](ResolveStatus status) {
        return status == ResolveStatus::Ambiguous ? "ambiguous" : "lost";
    };
    if (a.status != ResolveStatus::Resolved || b.status != ResolveStatus::Resolved) {
        *error = std::string("a face this edge lies between could not be found again (") +
                 describe(a.status != ResolveStatus::Resolved ? a.status : b.status) + ")";
        return false;
    }
    // The edge the two resolved faces share. Two faces of a solid share
    // at most one edge unless they meet twice, which is why finding more
    // than one is reported rather than picked from.
    std::vector<EntityId> shared;
    auto edges_of = [&](EntityId face_id) {
        std::vector<EntityId> ids;
        const Face *face = model.GetFace(face_id);
        if (face == nullptr) return ids;
        for (EntityId loop_id : face->loops) {
            const Loop *loop = model.GetLoop(loop_id);
            if (loop == nullptr) continue;
            for (EntityId coedge_id : loop->coedges) {
                const CoEdge *coedge = model.GetCoEdge(coedge_id);
                if (coedge != nullptr) ids.push_back(coedge->edge);
            }
        }
        return ids;
    };
    const std::vector<EntityId> first = edges_of(a.entity);
    const std::vector<EntityId> second = edges_of(b.entity);
    for (EntityId edge : first) {
        if (std::find(second.begin(), second.end(), edge) == second.end()) continue;
        if (std::find(shared.begin(), shared.end(), edge) == shared.end()) shared.push_back(edge);
    }
    (void)body;
    if (shared.size() != 1) {
        *error = "the two faces this edge lies between now share " + std::to_string(shared.size()) +
                 " edges, so which one was meant is no longer clear";
        return false;
    }
    *out = shared.front();
    return true;
}

// --- Blends: fillets and chamfers (Part E.3) ---------------------------
//
// The whole operation is local. Nothing outside the four faces around the
// edge is looked at, let alone classified, and no surface is intersected
// with one it is tangent to -- which is the entire reason this is not
// done as a boolean. See the header for the post-mortem of the attempt
// that was.

namespace {

// Everything the surgery needs to know about one edge, worked out once
// and checked once, so that the rebuild below can be pure bookkeeping.
struct BlendSite {
    EntityId edge = kNoEntity;
    EntityId vertex[2] = {kNoEntity, kNoEntity};  // the edge's two ends
    Vec3d point[2];
    Vec3d direction;  // unit, from end 0 to end 1
    double length = 0.0;

    EntityId face[2] = {kNoEntity, kNoEntity};  // the two faces along it
    Vec3d normal[2];                            // their *outward* normals
    Vec3d into[2];                              // into each face, square to the edge

    // Which way the material turns at this edge. A convex blend takes
    // material away, a concave one puts it back; the two differ by the
    // side the ball rolls on and by nothing else.
    bool convex = true;

    EntityId end_face[2] = {kNoEntity, kNoEntity};  // the face across each end
    // adjacent[f][k] is the edge of face[f] meeting this one at end k, and
    // the parameter along it that the blend trims it back to.
    EntityId adjacent[2][2] = {{kNoEntity, kNoEntity}, {kNoEntity, kNoEntity}};
    double adjacent_param[2][2] = {{0.0, 0.0}, {0.0, 0.0}};

    Vec3d centre[2];       // the rolling ball's axis, at each end
    Vec3d tangent[2][2];   // tangent[f][k]: where the blend meets face[f] at end k
};

// The average of samples along a face's outer boundary. Used only to ask
// which side of the edge the face is on -- see the note at its one call
// site for why that question cannot be answered from the normals.
Vec3d OuterBoundaryCentroid(const Model &model, EntityId face) {
    const Face *f = model.GetFace(face);
    if (f == nullptr) return Vec3d{};
    Vec3d sum;
    int count = 0;
    for (EntityId loop_id : f->loops) {
        const Loop *loop = model.GetLoop(loop_id);
        if (loop == nullptr || !loop->is_outer) continue;
        for (EntityId coedge_id : loop->coedges) {
            const CoEdge *coedge = model.GetCoEdge(coedge_id);
            if (coedge == nullptr) continue;
            const Edge *edge = model.GetEdge(coedge->edge);
            const Curve3 *curve = edge != nullptr ? model.CurveAt(edge->curve) : nullptr;
            if (curve == nullptr) continue;
            for (int i = 0; i < 8; ++i) {
                const double s = static_cast<double>(i) / 8.0;
                sum = sum + curve->Point(edge->t_start + (edge->t_end - edge->t_start) * s);
                ++count;
            }
        }
    }
    return count > 0 ? sum * (1.0 / static_cast<double>(count)) : Vec3d{};
}

// The face's outward normal, which for a plane is a constant and for a
// reversed face is the surface's own normal flipped.
bool PlanarFaceNormal(const Model &model, EntityId face, Vec3d *out) {
    const Face *f = model.GetFace(face);
    if (f == nullptr) return false;
    const auto *plane = dynamic_cast<const PlaneSurface *>(model.SurfaceAt(f->surface));
    if (plane == nullptr) return false;
    Vec3d n = plane->PlaneNormal().Normalized();
    if (f->orientation == Orientation::Reversed) n = n * -1.0;
    *out = n;
    return true;
}

// The two edges of `face` that meet `edge` at its two ends, found by
// walking the loop rather than by searching: a face can touch an edge's
// end vertex somewhere else entirely, and the one that matters is the one
// next to it in the traversal.
bool NeighboursInFace(const Model &model, EntityId face, EntityId edge, EntityId v0,
                      EntityId *at_v0, EntityId *at_v1, std::string *error) {
    const Face *f = model.GetFace(face);
    if (f == nullptr) {
        *error = "the face along the edge is missing";
        return false;
    }
    for (EntityId loop_id : f->loops) {
        const Loop *loop = model.GetLoop(loop_id);
        if (loop == nullptr) continue;
        const std::size_t n = loop->coedges.size();
        for (std::size_t i = 0; i < n; ++i) {
            const CoEdge *coedge = model.GetCoEdge(loop->coedges[i]);
            if (coedge == nullptr || coedge->edge != edge) continue;
            if (n < 3) {
                *error = "the loop along this edge has fewer than three sides, so there is no corner "
                         "to trim back to";
                return false;
            }
            const CoEdge *before = model.GetCoEdge(loop->coedges[(i + n - 1) % n]);
            const CoEdge *after = model.GetCoEdge(loop->coedges[(i + 1) % n]);
            if (before == nullptr || after == nullptr) {
                *error = "the loop along this edge is broken";
                return false;
            }
            const bool forward = model.CoEdgeStartVertex(loop->coedges[i]) == v0;
            *at_v0 = forward ? before->edge : after->edge;
            *at_v1 = forward ? after->edge : before->edge;
            if (*at_v0 == edge || *at_v1 == edge || *at_v0 == *at_v1) {
                *error = "this edge meets itself around its face, which a blend cannot trim";
                return false;
            }
            return true;
        }
    }
    *error = "the edge is not in any loop of the face that claims it";
    return false;
}

// The other face using `edge`.
EntityId FaceAcross(const Model &model, EntityId edge, EntityId known) {
    for (EntityId f : model.FacesOfEdge(edge)) {
        if (f != known) return f;
    }
    return kNoEntity;
}

// Where along `edge` the blend trims it back to, and -- just as much the
// point -- which of the two ways it can fail it failed.
//
// Note what is NOT used here: Curve3::ClosestPoint clamps to the curve's
// domain rather than reporting that it ran off the end, so a target well
// past the far end comes back as the far endpoint and reads as "this
// edge does not point the right way" when the truth is "this radius does
// not fit". Both are refusals, but only one of them tells the user to
// type a smaller number, so the arithmetic is done here where the two
// can be told apart.
bool TrimParameter(const Model &model, EntityId edge, EntityId vertex, const Vec3d &target,
                   const Tolerance &tolerance, double *out, std::string *error) {
    const Edge *e = model.GetEdge(edge);
    const Curve3 *curve = e != nullptr ? model.CurveAt(e->curve) : nullptr;
    if (curve == nullptr) {
        *error = "an edge next to the blend has no curve";
        return false;
    }
    const double drop = e->start_vertex == vertex ? e->t_start : e->t_end;
    const double keep = e->start_vertex == vertex ? e->t_end : e->t_start;
    const Vec3d corner = curve->Point(drop);
    const Vec3d far = curve->Point(keep);
    const Vec3d span = far - corner;
    const double span_length = span.Length();
    if (curve->Kind() != CurveKind::Line || !(span_length > 0.0)) {
        *error = "the edge next to the blend is not straight, so the blend surface would have to be "
                 "intersected with the face beyond it rather than simply stopping there";
        return false;
    }
    const Vec3d direction = span * (1.0 / span_length);
    const Vec3d relative = target - corner;
    const double along = relative.Dot(direction);
    if (!tolerance.SamePoint(corner + direction * along, target)) {
        *error = "the edge next to the blend does not run away from the corner in the direction the "
                 "blend trims, so there is no point on it to trim back to";
        return false;
    }
    const double fraction = along / span_length;
    if (fraction >= 1.0 - 1e-9) {
        *error = "the blend is too large for the face next to it: trimming back that far would run "
                 "past the far end of a neighbouring edge";
        return false;
    }
    if (fraction <= 1e-9) {
        *error = "the blend does not reach into the face next to it";
        return false;
    }
    *out = drop + (keep - drop) * fraction;
    return true;
}

bool DescribeBlendSite(const Model &model, EntityId body, EntityId edge, const BlendOptions &options,
                       BlendSite *site, std::string *error) {
    const Body *b = model.GetBody(body);
    if (b == nullptr) {
        *error = "no such body";
        return false;
    }
    if (!(options.radius > 0.0)) {
        *error = "a blend needs a positive radius";
        return false;
    }
    const Edge *e = model.GetEdge(edge);
    if (e == nullptr) {
        *error = "no such edge";
        return false;
    }
    if (e->IsClosed()) {
        *error = "a closed edge has no two ends for the blend to stop at";
        return false;
    }
    const Curve3 *curve = model.CurveAt(e->curve);
    if (curve == nullptr || curve->Kind() != CurveKind::Line) {
        *error = "this blend handles straight edges only; a curved edge needs the blend surface to "
                 "be intersected with its neighbours, which is a later piece of work";
        return false;
    }

    site->edge = edge;
    site->vertex[0] = e->start_vertex;
    site->vertex[1] = e->end_vertex;
    site->point[0] = model.EdgeStartPoint(edge);
    site->point[1] = model.EdgeEndPoint(edge);
    const Vec3d along = site->point[1] - site->point[0];
    site->length = along.Length();
    if (!(site->length > 0.0)) {
        *error = "the edge has no length";
        return false;
    }
    site->direction = along * (1.0 / site->length);

    const std::vector<EntityId> faces = model.FacesOfEdge(edge);
    if (faces.size() != 2) {
        *error = "this edge is used by " + std::to_string(faces.size()) +
                 " faces; a blend needs exactly two";
        return false;
    }
    site->face[0] = faces[0];
    site->face[1] = faces[1];
    for (int f = 0; f < 2; ++f) {
        if (!PlanarFaceNormal(model, site->face[f], &site->normal[f])) {
            *error = "this blend handles planar faces only, and the face on one side of the edge is "
                     "not a plane";
            return false;
        }
    }
    if (site->normal[0].Cross(site->normal[1]).Length() <= 1e-9) {
        *error = "the two faces along this edge are parallel, so there is no corner to blend";
        return false;
    }

    // WHICH SIDE THE FACE IS ON cannot be read off the two normals. A
    // box's convex corner and an L's reflex corner present the very same
    // pair of normals -- what differs is which way each face extends from
    // the edge, and that has to be asked of the face's own boundary.
    const Vec3d middle = (site->point[0] + site->point[1]) * 0.5;
    for (int f = 0; f < 2; ++f) {
        const Vec3d across = site->normal[f].Cross(site->direction).Normalized();
        const double side = (OuterBoundaryCentroid(model, site->face[f]) - middle).Dot(across);
        if (std::fabs(side) <= 1e-9) {
            *error = "the face along this edge straddles it, so which way it extends is not clear";
            return false;
        }
        site->into[f] = side > 0.0 ? across : across * -1.0;
    }
    site->convex = site->into[0].Dot(site->normal[1]) < 0.0;

    // The rolling ball. Its centre is the point at distance `radius` from
    // both planes, square to the edge -- on the material side for a
    // convex blend, which removes material, and in the void for a concave
    // one, which adds it. That single sign is the whole difference.
    const double sign = site->convex ? -1.0 : 1.0;
    const Mat3d frame = Mat3d::FromRows(site->normal[0], site->normal[1], site->direction);
    Vec3d offset;
    if (!frame.Solve(Vec3d{sign * options.radius, sign * options.radius, 0.0}, &offset)) {
        *error = "the rolling ball's centre is not determined at this edge";
        return false;
    }
    for (int k = 0; k < 2; ++k) site->centre[k] = site->point[k] + offset;

    // Where the blend meets each face. For a fillet that is where the
    // ball touches, which is only `radius` from the edge when the faces
    // meet squarely and is further when they meet at a sharper angle. For
    // a chamfer `radius` is the setback itself, which is what a chamfer
    // is specified by.
    for (int f = 0; f < 2; ++f) {
        for (int k = 0; k < 2; ++k) {
            site->tangent[f][k] = options.kind == BlendKind::Fillet
                                      ? site->centre[k] - site->normal[f] * (sign * options.radius)
                                      : site->point[k] + site->into[f] * options.radius;
        }
        // A cross-check of the side test above: the blend has to land
        // inside the face it trims, not behind it.
        if ((site->tangent[f][0] - site->point[0]).Dot(site->into[f]) <= 0.0) {
            *error = "the blend lands outside the face it would trim";
            return false;
        }
    }

    // The faces across the edge's two ends. The blend's cross-section
    // lives in each of them, so each has to be a plane square to the
    // edge -- otherwise the cross-section is not a circle and the end
    // needs the blend surface intersected with whatever is there.
    for (int k = 0; k < 2; ++k) {
        for (int f = 0; f < 2; ++f) {
            EntityId at_v0 = kNoEntity;
            EntityId at_v1 = kNoEntity;
            if (!NeighboursInFace(model, site->face[f], edge, site->vertex[0], &at_v0, &at_v1, error)) {
                return false;
            }
            site->adjacent[f][0] = at_v0;
            site->adjacent[f][1] = at_v1;
        }
        const EntityId across_a = FaceAcross(model, site->adjacent[0][k], site->face[0]);
        const EntityId across_b = FaceAcross(model, site->adjacent[1][k], site->face[1]);
        if (across_a == kNoEntity || across_a != across_b) {
            *error = "the two faces along this edge do not meet a single common face at one of its "
                     "ends, so the blend has nothing to stop against there";
            return false;
        }
        site->end_face[k] = across_a;
        Vec3d end_normal;
        if (!PlanarFaceNormal(model, site->end_face[k], &end_normal)) {
            *error = "the face across one end of this edge is not a plane";
            return false;
        }
        if (std::fabs(end_normal.Dot(site->direction)) < 1.0 - 1e-9) {
            *error = "the face across one end of this edge is not square to it; a blend that runs "
                     "out onto a slanted face needs its surface intersected with that face, which "
                     "is a later piece of work";
            return false;
        }
    }
    if (site->adjacent[0][0] == site->adjacent[1][0] || site->adjacent[0][1] == site->adjacent[1][1]) {
        *error = "the two faces along this edge share another edge at one of its ends";
        return false;
    }

    for (int f = 0; f < 2; ++f) {
        for (int k = 0; k < 2; ++k) {
            if (!TrimParameter(model, site->adjacent[f][k], site->vertex[k], site->tangent[f][k],
                               options.tolerance, &site->adjacent_param[f][k], error)) {
                return false;
            }
        }
    }
    return true;
}

// One trim to apply while copying: this edge, at this end, now stops at
// this new vertex and this new parameter.
struct EdgeTrim {
    EntityId edge = kNoEntity;
    EntityId vertex = kNoEntity;
    EntityId replacement = kNoEntity;
    double parameter = 0.0;
};

bool StitchBlend(const Model &src, EntityId body, const BlendSite &site, const BlendOptions &options,
                 Model *out, EntityId *out_body, std::string *error) {
    const Body *src_body = src.GetBody(body);
    const Face *first_face = src.GetFace(site.face[0]);
    if (src_body == nullptr || first_face == nullptr) {
        *error = "the body went missing between describing the blend and building it";
        return false;
    }
    const EntityId blend_shell = first_face->shell;

    // --- The blend's own geometry --------------------------------------
    EntityId tangent_vertex[2][2];
    for (int f = 0; f < 2; ++f) {
        for (int k = 0; k < 2; ++k) tangent_vertex[f][k] = out->AddVertex(site.tangent[f][k]);
    }

    // The two lines where the blend meets the faces it trims. Each runs
    // from the edge's end 0 to its end 1, which is the direction the
    // original edge ran, so a loop that used the original edge keeps its
    // coedge's orientation unchanged.
    EntityId tangent_edge[2];
    for (int f = 0; f < 2; ++f) {
        const int c = out->AddCurve(
            std::make_shared<Line3>(Line3::FromPoints(site.tangent[f][0], site.tangent[f][1])));
        tangent_edge[f] = out->AddEdge(c, tangent_vertex[f][0], tangent_vertex[f][1], 0.0, 1.0);
    }

    // The frame the blend surface and both its cross-sections share: u
    // measured about the ball's axis from the first face's tangent line.
    const Vec3d x_axis = (site.tangent[0][0] - site.centre[0]).Normalized();
    const Vec3d y_axis = site.direction.Cross(x_axis).Normalized();
    const Vec3d to_second = (site.tangent[1][0] - site.centre[0]).Normalized();
    const double sweep = std::atan2(to_second.Dot(y_axis), to_second.Dot(x_axis));
    if (!(std::fabs(sweep) > 1e-9) || std::fabs(sweep) > kPi - 1e-9) {
        *error = "the two faces meet at an angle the blend cannot span";
        return false;
    }
    const double chord = (site.tangent[1][0] - site.tangent[0][0]).Length();

    // The cross-sections: an arc of the rolling ball for a fillet, the
    // chord itself for a chamfer. Both run from the first face's tangent
    // point to the second's.
    EntityId cross_edge[2];
    for (int k = 0; k < 2; ++k) {
        if (options.kind == BlendKind::Fillet) {
            const int c = out->AddCurve(std::make_shared<Circle3>(site.centre[k], x_axis, y_axis,
                                                                  options.radius, std::min(0.0, sweep),
                                                                  std::max(0.0, sweep)));
            cross_edge[k] = out->AddEdge(c, tangent_vertex[0][k], tangent_vertex[1][k], 0.0, sweep);
        } else {
            const int c = out->AddCurve(
                std::make_shared<Line3>(Line3::FromPoints(site.tangent[0][k], site.tangent[1][k])));
            cross_edge[k] = out->AddEdge(c, tangent_vertex[0][k], tangent_vertex[1][k], 0.0, 1.0);
        }
    }

    // The blend surface, and the rectangle its boundary is in the
    // surface's own (u, v). A loop runs counter-clockwise there whatever
    // the face's orientation turns out to be -- that is the convention
    // everything else in this file builds to.
    std::shared_ptr<const Surface> surface;
    std::vector<EntityId> blend_coedges;
    if (options.kind == BlendKind::Fillet) {
        surface = std::make_shared<CylinderSurface>(site.centre[0], x_axis, site.direction, options.radius,
                                                    std::min(0.0, sweep), std::max(0.0, sweep), 0.0,
                                                    site.length);
        if (sweep > 0.0) {
            // u runs from the first face's tangent line to the second's.
            blend_coedges = {out->AddCoEdge(cross_edge[0], Orientation::Forward),
                             out->AddCoEdge(tangent_edge[1], Orientation::Forward),
                             out->AddCoEdge(cross_edge[1], Orientation::Reversed),
                             out->AddCoEdge(tangent_edge[0], Orientation::Reversed)};
        } else {
            blend_coedges = {out->AddCoEdge(cross_edge[0], Orientation::Reversed),
                             out->AddCoEdge(tangent_edge[0], Orientation::Forward),
                             out->AddCoEdge(cross_edge[1], Orientation::Forward),
                             out->AddCoEdge(tangent_edge[1], Orientation::Reversed)};
        }
    } else {
        surface = std::make_shared<PlaneSurface>(site.tangent[0][0],
                                                 (site.tangent[1][0] - site.tangent[0][0]).Normalized(),
                                                 site.direction, 0.0, chord, 0.0, site.length);
        blend_coedges = {out->AddCoEdge(cross_edge[0], Orientation::Forward),
                         out->AddCoEdge(tangent_edge[1], Orientation::Forward),
                         out->AddCoEdge(cross_edge[1], Orientation::Reversed),
                         out->AddCoEdge(tangent_edge[0], Orientation::Reversed)};
    }

    // WHICH WAY THE BLEND FACES. A convex blend took material away, so
    // what is left lies on the far side of the blend from the edge it
    // replaced, and the outward normal points back towards that edge. A
    // concave blend put material in, so it points away. One test, both
    // kinds, both convexities -- and no chance of quietly getting the
    // sign of a cross product backwards.
    double u_lo = 0.0;
    double u_hi = 0.0;
    double v_lo = 0.0;
    double v_hi = 0.0;
    surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    const Vec3d mid = surface->Point(0.5 * (u_lo + u_hi), 0.5 * (v_lo + v_hi));
    const Vec3d surface_normal = surface->Normal(0.5 * (u_lo + u_hi), 0.5 * (v_lo + v_hi));
    const Vec3d edge_middle = (site.point[0] + site.point[1]) * 0.5;
    const Vec3d outward = site.convex ? (edge_middle - mid) : (mid - edge_middle);
    const Orientation blend_orientation =
        surface_normal.Dot(outward) > 0.0 ? Orientation::Forward : Orientation::Reversed;
    const EntityId blend_face =
        out->AddFace(out->AddSurface(surface), blend_orientation, {out->AddLoop(blend_coedges, true)},
                     options.kind == BlendKind::Fillet ? "fillet" : "chamfer");

    // --- Everything else, copied with four edges trimmed ---------------
    std::vector<EdgeTrim> trims;
    for (int f = 0; f < 2; ++f) {
        for (int k = 0; k < 2; ++k) {
            trims.push_back(EdgeTrim{site.adjacent[f][k], site.vertex[k], tangent_vertex[f][k],
                                     site.adjacent_param[f][k]});
        }
    }

    std::vector<int> curve_map(static_cast<std::size_t>(src.CurveCount()), -1);
    std::vector<int> surface_map(static_cast<std::size_t>(src.SurfaceCount()), -1);
    std::vector<EntityId> vertex_map(src.Vertices().size(), kNoEntity);
    std::vector<EntityId> edge_map(src.Edges().size(), kNoEntity);

    auto map_curve = [&](int index) {
        int &slot = curve_map[static_cast<std::size_t>(index)];
        if (slot < 0) slot = out->AddCurve(std::shared_ptr<const Curve3>(src.CurveAt(index)->Clone().release()));
        return slot;
    };
    auto map_surface = [&](int index) {
        int &slot = surface_map[static_cast<std::size_t>(index)];
        if (slot < 0) {
            slot = out->AddSurface(std::shared_ptr<const Surface>(src.SurfaceAt(index)->Clone().release()));
        }
        return slot;
    };
    auto map_vertex = [&](EntityId id) {
        EntityId &slot = vertex_map[static_cast<std::size_t>(id)];
        if (slot == kNoEntity) {
            const Vertex *v = src.GetVertex(id);
            slot = out->AddVertex(v->point, v->tolerance);
        }
        return slot;
    };
    auto map_edge = [&](EntityId id) {
        EntityId &slot = edge_map[static_cast<std::size_t>(id)];
        if (slot != kNoEntity) return slot;
        const Edge *e = src.GetEdge(id);
        EntityId start = kNoEntity;
        EntityId end = kNoEntity;
        double t_start = e->t_start;
        double t_end = e->t_end;
        for (const EdgeTrim &trim : trims) {
            if (trim.edge != id) continue;
            if (e->start_vertex == trim.vertex) {
                start = trim.replacement;
                t_start = trim.parameter;
            } else if (e->end_vertex == trim.vertex) {
                end = trim.replacement;
                t_end = trim.parameter;
            }
        }
        if (start == kNoEntity) start = map_vertex(e->start_vertex);
        if (end == kNoEntity) end = map_vertex(e->end_vertex);
        slot = out->AddEdge(map_curve(e->curve), start, end, t_start, t_end, e->tolerance);
        return slot;
    };

    std::vector<EntityId> new_shells;
    for (EntityId shell_id : src_body->shells) {
        const Shell *shell = src.GetShell(shell_id);
        if (shell == nullptr) continue;
        std::vector<EntityId> new_faces;
        for (EntityId face_id : shell->faces) {
            const Face *face = src.GetFace(face_id);
            if (face == nullptr) continue;
            int side = -1;
            if (face_id == site.face[0]) side = 0;
            else if (face_id == site.face[1]) side = 1;
            int end = -1;
            if (face_id == site.end_face[0]) end = 0;
            else if (face_id == site.end_face[1]) end = 1;

            std::vector<EntityId> new_loops;
            for (EntityId loop_id : face->loops) {
                const Loop *loop = src.GetLoop(loop_id);
                if (loop == nullptr) continue;
                const std::size_t n = loop->coedges.size();
                std::vector<EntityId> coedges;
                for (std::size_t i = 0; i < n; ++i) {
                    const CoEdge *coedge = src.GetCoEdge(loop->coedges[i]);
                    if (side >= 0 && coedge->edge == site.edge) {
                        // The blended edge, traded for the line the blend
                        // meets this face along.
                        coedges.push_back(out->AddCoEdge(tangent_edge[side], coedge->orientation));
                    } else {
                        coedges.push_back(out->AddCoEdge(map_edge(coedge->edge), coedge->orientation));
                    }
                    if (end < 0) continue;
                    // The corner where the two trimmed edges used to meet,
                    // traded for the blend's cross-section. The corner
                    // vertex itself is left behind: nothing references it
                    // any more.
                    const EntityId next = loop->coedges[(i + 1) % n];
                    if (src.CoEdgeEndVertex(loop->coedges[i]) != site.vertex[end] ||
                        src.CoEdgeStartVertex(next) != site.vertex[end]) {
                        continue;
                    }
                    const bool from_first = out->CoEdgeEndVertex(coedges.back()) == tangent_vertex[0][end];
                    coedges.push_back(out->AddCoEdge(
                        cross_edge[end], from_first ? Orientation::Forward : Orientation::Reversed));
                }
                new_loops.push_back(out->AddLoop(coedges, loop->is_outer));
            }
            new_faces.push_back(out->AddFace(map_surface(face->surface), face->orientation, new_loops,
                                             face->name, face->tolerance));
        }
        if (shell_id == blend_shell) new_faces.push_back(blend_face);
        new_shells.push_back(out->AddShell(new_faces, shell->is_outer));
    }
    *out_body = out->AddBody(new_shells, src_body->name);

    PCurveOptions pcurve_options;
    pcurve_options.tolerance = options.tolerance;
    if (!BuildAllPCurves(out, pcurve_options, error)) {
        *error = "the blended body's p-curves could not be built: " + *error;
        return false;
    }
    return true;
}

}  // namespace

bool BlendEdge(const Model &model, EntityId body, EntityId edge, const BlendOptions &options, Model *out,
               EntityId *out_body, std::string *error) {
    error->clear();
    *out = Model();
    *out_body = kNoEntity;
    BlendSite site;
    if (!DescribeBlendSite(model, body, edge, options, &site, error)) return false;
    return StitchBlend(model, body, site, options, out, out_body, error);
}

bool BlendEdges(const Model &model, EntityId body, const std::vector<EntityId> &edges,
                const BlendOptions &options, Model *out, EntityId *out_body, std::string *error) {
    error->clear();
    if (edges.empty()) {
        *error = "a blend needs at least one edge";
        return false;
    }
    // Name every edge up front, against the model they were picked in.
    // After the first blend the ids mean nothing, and the face pair is
    // the only description that survives.
    std::vector<std::pair<EntityName, EntityName>> named;
    for (EntityId e : edges) {
        EntityName a;
        EntityName b;
        if (!CaptureEdgeBetweenFaces(model, e, -1, &a, &b)) {
            *error = "edge " + std::to_string(e) + " does not lie between two faces";
            return false;
        }
        named.emplace_back(a, b);
    }

    Model current;
    EntityId current_body = kNoEntity;
    for (std::size_t i = 0; i < edges.size(); ++i) {
        const Model &source = i == 0 ? model : current;
        const EntityId source_body = i == 0 ? body : current_body;
        EntityId target = edges[0];
        if (i > 0 && !ResolveEdgeBetweenFaces(source, source_body, named[i].first, named[i].second,
                                              &target, error)) {
            *error = "blend " + std::to_string(i + 1) + " of " + std::to_string(edges.size()) +
                     " could not find its edge again after the earlier ones: " + *error;
            return false;
        }
        Model next;
        EntityId next_body = kNoEntity;
        if (!BlendEdge(source, source_body, target, options, &next, &next_body, error)) {
            *error = "blend " + std::to_string(i + 1) + " of " + std::to_string(edges.size()) + ": " +
                     *error;
            return false;
        }
        current = std::move(next);
        current_body = next_body;
    }
    *out = std::move(current);
    *out_body = current_body;
    return true;
}

// --- The tree ----------------------------------------------------------

FeatureTree::FeatureTree() = default;

int FeatureTree::AddSketch(const Sketch &sketch, const std::string &name) {
    Feature feature;
    feature.kind = FeatureKind::Sketch;
    feature.name = name.empty() ? "sketch" : name;
    feature.sketch_geometry = sketch;
    return AddFeature(feature);
}

int FeatureTree::AddFeature(const Feature &feature) {
    Feature copy = feature;
    copy.id = next_id_++;
    features_.push_back(copy);
    results_.emplace_back();
    steps_.emplace_back();
    first_dirty_ = std::min(first_dirty_, static_cast<int>(features_.size()) - 1);
    return copy.id;
}

bool FeatureTree::RemoveFeature(int id) {
    for (std::size_t i = 0; i < features_.size(); ++i) {
        if (features_[i].id != id) continue;
        features_.erase(features_.begin() + static_cast<std::ptrdiff_t>(i));
        results_.erase(results_.begin() + static_cast<std::ptrdiff_t>(i));
        steps_.erase(steps_.begin() + static_cast<std::ptrdiff_t>(i));
        first_dirty_ = std::min(first_dirty_, static_cast<int>(i));
        return true;
    }
    return false;
}

const Feature *FeatureTree::Get(int id) const {
    for (const Feature &feature : features_) {
        if (feature.id == id) return &feature;
    }
    return nullptr;
}

Feature *FeatureTree::GetMutable(int id) {
    for (std::size_t i = 0; i < features_.size(); ++i) {
        if (features_[i].id != id) continue;
        // Handing out a mutable feature is itself the change: the caller
        // has no way to tell the tree afterwards that would not be
        // forgotten about half the time.
        first_dirty_ = std::min(first_dirty_, static_cast<int>(i));
        return &features_[i];
    }
    return nullptr;
}

void FeatureTree::MarkDirty(int id) {
    for (std::size_t i = 0; i < features_.size(); ++i) {
        if (features_[i].id == id) first_dirty_ = std::min(first_dirty_, static_cast<int>(i));
    }
}

const FeatureResult *FeatureTree::ResultOf(int id) const {
    for (std::size_t i = 0; i < features_.size(); ++i) {
        if (features_[i].id == id) return &results_[i];
    }
    return nullptr;
}

double FeatureTree::Volume() const {
    double total = 0.0;
    for (EntityId body : bodies_) {
        TessellationMesh mesh;
        std::string error;
        if (TessellateBody(model_, body, {}, &mesh, &error)) total += mesh.SignedVolume();
    }
    return total;
}

bool FeatureTree::Rebuild(std::string *error) {
    error->clear();
    const int start = std::max(0, std::min(first_dirty_, static_cast<int>(features_.size())));
    bool all_ok = true;

    Model carried;
    std::vector<EntityId> carried_bodies;
    if (start > 0) {
        carried = steps_[static_cast<std::size_t>(start - 1)].model;
        carried_bodies = steps_[static_cast<std::size_t>(start - 1)].bodies;
    }

    for (std::size_t i = static_cast<std::size_t>(start); i < features_.size(); ++i) {
        const Feature &feature = features_[i];
        FeatureResult &result = results_[i];
        result = FeatureResult{};

        if (feature.suppressed || feature.kind == FeatureKind::Sketch) {
            // A sketch produces no geometry of its own, and a suppressed
            // feature produces none by request. Both carry the incoming
            // state forward unchanged so that the feature after them sees
            // what it would have seen.
            steps_[i].model = carried;
            steps_[i].bodies = carried_bodies;
            result.ok = true;
            result.bodies = carried_bodies;
            continue;
        }

        Step step;
        std::string feature_error;
        if (Evaluate(feature, carried, carried_bodies, &step, &feature_error)) {
            steps_[i] = std::move(step);
            result.ok = true;
            result.bodies = steps_[i].bodies;
        } else {
            // The failure is kept on the feature and the rebuild carries
            // on. A tree that stops at the first broken feature tells you
            // about one problem at a time, and a part with three broken
            // features then takes three rebuilds to understand.
            result.ok = false;
            result.error = feature_error;
            steps_[i].model = carried;
            steps_[i].bodies = carried_bodies;
            all_ok = false;
            if (error->empty()) *error = feature.name.empty() ? feature_error
                                                              : feature.name + ": " + feature_error;
        }
        carried = steps_[i].model;
        carried_bodies = steps_[i].bodies;
    }

    model_ = carried;
    bodies_ = carried_bodies;
    for (std::size_t i = 0; i < results_.size(); ++i) {
        if (!results_[i].ok) continue;
        results_[i].volume = 0.0;
        for (EntityId body : steps_[i].bodies) {
            TessellationMesh mesh;
            std::string mesh_error;
            if (TessellateBody(steps_[i].model, body, {}, &mesh, &mesh_error)) {
                results_[i].volume += mesh.SignedVolume();
            }
        }
    }
    first_dirty_ = static_cast<int>(features_.size());
    return all_ok;
}

// A tolerance suited to the part in hand rather than to the one the
// default was chosen for.
//
// cad_math.h's Tolerance says in as many words that its 1e-7 default is
// "the value most kernels settle on for models measured in millimetres",
// and that one global epsilon is the classic way a kernel that works on
// its own primitives falls over. A part measured in *metres* is exactly
// that case. An extruded ellipse 3.25 m across has a p-curve that
// follows its edge to about a micron -- three parts in ten million of
// the part, and entirely respectable -- and a boolean against it leaves
// loops that close to a few microns. Against an absolute 1e-7 both are
// errors, the feature refuses to build, and nothing about the message
// says that the part was fine and the yardstick was not.
//
// Relative to the body's own diagonal, with the old absolute value as a
// floor so that nothing which passed before stops passing.
Tolerance ToleranceFor(const Model &model) {
    Tolerance tolerance;
    const Box3d bounds = model.Bounds();
    if (bounds.IsEmpty()) return tolerance;
    const double diagonal =
        std::sqrt(bounds.x.Width() * bounds.x.Width() + bounds.y.Width() * bounds.y.Width() +
                  bounds.z.Width() * bounds.z.Width());
    if (diagonal > 0.0) tolerance.linear = std::max(tolerance.linear, diagonal * 1e-6);
    return tolerance;
}

bool FeatureTree::Evaluate(const Feature &feature, const Model &incoming,
                           const std::vector<EntityId> &incoming_bodies, Step *out,
                           std::string *error) const {
    const Feature *sketch_feature = Get(feature.sketch);
    if (sketch_feature == nullptr || sketch_feature->kind != FeatureKind::Sketch) {
        *error = "no sketch to build from";
        return false;
    }
    const Sketch &sketch = sketch_feature->sketch_geometry;
    Sketch::Profile profile;
    if (!SelectProfile(sketch, feature.profile_index, &profile, error)) return false;

    Model built;
    EntityId built_body = kNoEntity;
    switch (feature.kind) {
        case FeatureKind::Extrude: {
            Vec3d direction = sketch.Plane().Normal();
            if (feature.reverse) direction = -direction;
            double distance = feature.distance;
            Vec3d start = Vec3d{0.0, 0.0, 0.0};
            if (feature.end == ExtrudeEnd::Symmetric) {
                // Built from half a distance below the sketch plane, so
                // the plane ends up in the middle rather than at one end.
                start = -direction * (feature.distance * 0.5);
                distance = feature.distance;
            } else if (feature.end == ExtrudeEnd::ThroughAll) {
                // Far enough to clear whatever is there, worked out from
                // the target's own extent rather than from a big number:
                // a constant large enough for a part in millimetres is
                // not large enough for one in microns, and one large
                // enough for both wrecks the conditioning of every
                // intersection it takes part in.
                Box3d extent;
                for (EntityId body : incoming_bodies) {
                    const Body *target = incoming.GetBody(body);
                    if (target == nullptr) continue;
                    for (EntityId shell_id : target->shells) {
                        const Shell *shell = incoming.GetShell(shell_id);
                        if (shell == nullptr) continue;
                        for (EntityId face_id : shell->faces) {
                            const Face *face = incoming.GetFace(face_id);
                            const Surface *surface =
                                face != nullptr ? incoming.SurfaceAt(face->surface) : nullptr;
                            if (surface != nullptr) extent.Expand(surface->Bounds());
                        }
                    }
                }
                if (extent.IsEmpty()) {
                    start = -direction * std::fabs(feature.distance);
                    distance = std::fabs(feature.distance) * 2.0;
                } else {
                    // Exactly far enough, measured along the extrude's own
                    // direction, plus a small margin at each end.
                    //
                    // A large round number instead is the obvious version
                    // and it is quietly destructive: a tool four times the
                    // part's diagonal makes every intersection it takes
                    // part in that much worse conditioned, and the
                    // endpoints of a hole's rim came back three microns
                    // apart -- far enough for the stitch to treat them as
                    // two vertices and leave the shell open.
                    double lowest = 0.0;
                    double highest = 0.0;
                    bool first = true;
                    for (int corner = 0; corner < 8; ++corner) {
                        const Vec3d point{(corner & 1) ? extent.x.hi : extent.x.lo,
                                          (corner & 2) ? extent.y.hi : extent.y.lo,
                                          (corner & 4) ? extent.z.hi : extent.z.lo};
                        const double along = (point - sketch.Plane().origin).Dot(direction);
                        lowest = first ? along : std::min(lowest, along);
                        highest = first ? along : std::max(highest, along);
                        first = false;
                    }
                    const double margin = std::max(1e-6, (highest - lowest) * 0.05);
                    start = direction * (lowest - margin);
                    distance = (highest - lowest) + 2.0 * margin;
                }
            }
            // A cutting tool is pushed a hair back past its own start.
            //
            // The commonest operation in CAD is a pocket sketched on the
            // face it starts from, and that puts the tool's own end cap
            // exactly on that face -- which Part C.4 refuses, because two
            // coincident faces need a code path it does not have. The
            // distance is lengthened by the same amount, so the floor of
            // the pocket stays where it was asked for and only the end
            // that was already outside the material moves.
            //
            // What this costs, stated rather than hidden: a cut sketched
            // on a plane with material *above* it reaches that far into
            // it. The margin is small enough for that to be far below any
            // tolerance a part is made to, and large enough to be well
            // clear of the coincidence test.
            if (feature.combine == FeatureCombine::Cut) {
                const double margin = std::max(1e-6, std::fabs(distance) * 1e-5);
                start = start - direction * margin;
                distance += margin;
            }
            Sketch shifted = sketch;
            shifted.Plane().origin = shifted.Plane().origin + start;
            if (!ExtrudeProfile(shifted, profile, direction, distance, &built, &built_body, error)) {
                return false;
            }
            break;
        }
        case FeatureKind::Revolve: {
            const SketchEntity *axis = sketch.GetEntity(feature.axis_entity);
            if (axis == nullptr || axis->kind != SketchEntityKind::Line || axis->points.size() < 2) {
                *error = "a revolve needs a line in the sketch to turn about";
                return false;
            }
            const SketchPoint *a = sketch.GetPoint(axis->points[0]);
            const SketchPoint *b = sketch.GetPoint(axis->points[1]);
            if (a == nullptr || b == nullptr) {
                *error = "the revolve axis line has no endpoints";
                return false;
            }
            if (!RevolveProfile(sketch, profile, a->position, b->position - a->position, feature.angle, &built,
                                &built_body, error)) {
                return false;
            }
            break;
        }
        case FeatureKind::Loft: {
            std::vector<const Sketch *> sketches;
            std::vector<Sketch::Profile> section_profiles;
            for (int section : feature.sections) {
                const Feature *s = Get(section);
                if (s == nullptr || s->kind != FeatureKind::Sketch) {
                    *error = "a lofted section is not a sketch";
                    return false;
                }
                Sketch::Profile section_profile;
                if (!SelectProfile(s->sketch_geometry, -1, &section_profile, error)) return false;
                sketches.push_back(&s->sketch_geometry);
                section_profiles.push_back(section_profile);
            }
            if (!LoftProfiles(sketches, section_profiles, &built, &built_body, error)) return false;
            break;
        }
        case FeatureKind::Sweep: {
            const Feature *spine_feature = Get(feature.spine);
            if (spine_feature == nullptr || spine_feature->kind != FeatureKind::Sketch) {
                *error = "a sweep needs a sketch holding its spine";
                return false;
            }
            const std::shared_ptr<const Curve3> spine =
                spine_feature->sketch_geometry.WorldCurve(feature.spine_entity);
            if (!spine) {
                *error = "a sweep needs a curve in the spine sketch to follow";
                return false;
            }
            if (!SweepProfile(sketch, profile, *spine, feature.spine_sections, feature.twist,
                              feature.end_scale, &built, &built_body, error)) {
                return false;
            }
            break;
        }
        case FeatureKind::Sketch:
            *error = "a sketch has no geometry of its own";
            return false;
    }

    {
        ValidationOptions options;
        options.tolerance = ToleranceFor(built);
        ValidationReport report;
        if (!ValidateBody(built, built_body, &report, options)) {
            *error = "the feature produced an invalid body:\n" + report.Summary();
            return false;
        }
    }

    if (feature.combine == FeatureCombine::NewBody || incoming_bodies.empty()) {
        // Nothing to combine with, whatever was asked for: a cut with no
        // body to cut is the feature's own body, which is almost never
        // what was meant, so it is refused rather than quietly turned
        // into a NewBody.
        if (feature.combine != FeatureCombine::NewBody && incoming_bodies.empty()) {
            *error = "there is no body to combine with";
            return false;
        }
        // The new body is copied into the running model so that
        // everything downstream sees one model rather than a list of them.
        Model merged = incoming;
        EntityId copied = kNoEntity;
        if (!CopyBodyInto(built, built_body, &merged, &copied, false, false, feature.name, error)) {
            return false;
        }
        // A copied body arrives without p-curves: CopyBodyInto rebuilds
        // the topology and the geometry, and a p-curve is neither -- it
        // is a derived thing belonging to a coedge on a particular
        // surface. Everything downstream that reads a face's boundary
        // needs them, tessellation included, so a body copied and not
        // rebuilt is a body with no volume.
        if (!BuildAllPCurves(&merged, {}, error)) return false;
        out->model = std::move(merged);
        out->bodies = incoming_bodies;
        out->bodies.push_back(copied);
        return true;
    }

    BooleanOp op = BooleanOp::Union;
    if (feature.combine == FeatureCombine::Cut) op = BooleanOp::Difference;
    if (feature.combine == FeatureCombine::Intersect) op = BooleanOp::Intersection;

    // Combined with the last body. A feature that should combine with a
    // particular one of several is a reference, and that belongs in E.6's
    // multi-body work rather than being guessed at here.
    Model result;
    BooleanReport report;
    // The looser of the two parts' tolerances: a small feature cut into a
    // large body has to stitch against the large body's edges, and
    // judging that by the small feature's own scale is judging it by a
    // yardstick the work it has to match was never held to.
    BooleanOptions boolean_options;
    const Tolerance incoming_tolerance = ToleranceFor(incoming);
    const Tolerance built_tolerance = ToleranceFor(built);
    boolean_options.tolerance.linear =
        std::max(incoming_tolerance.linear, built_tolerance.linear);
    if (!BooleanOperation(incoming, incoming_bodies.back(), built, built_body, op, &result, &report,
                          boolean_options)) {
        *error = report.error;
        return false;
    }
    out->model = std::move(result);
    out->bodies = std::vector<EntityId>{report.body};
    return true;
}

}  // namespace cad
