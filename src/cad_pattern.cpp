#include "cad_pattern.h"

#include "cad_pcurve.h"
#include "cad_validate.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace cad {
namespace {

// Whether a transformed surface still carries the same parameters over
// the same points.
//
// This is the subtle half of mirroring, and it splits by surface type
// rather than by anything a caller can see. A plane is stored as two
// axes and both of them reflect, so its (u, v) survives untouched: the
// same parameters name the reflected point. A cylinder is stored as an
// axis and *one* axis, with the second recovered as a cross product --
// which comes out the other way round under a reflection -- so its angle
// runs backwards afterwards, and the same face now occupies the negated
// half of its domain.
//
// The consequence is that the two need opposite corrections, and they
// cancel: where the parameters survive, the surface normal has flipped
// and the face's orientation flag must flip with it, while the loop is
// left alone. Where the parameters reversed, the normal came out
// unchanged and the flag stays, while the loop must be turned round to
// stay counter-clockwise in a parameter space that has been mirrored.
// Doing either correction in both cases yields a body that validates
// structurally and is inside out.
enum class Reparameterisation { Same, Reversed, Inconsistent };

Reparameterisation SenseOf(const Surface &before, const Surface &after, const Mat4d &transform,
                           const Tolerance &tolerance) {
    double u_lo = 0.0;
    double u_hi = 0.0;
    double v_lo = 0.0;
    double v_hi = 0.0;
    before.Domain(&u_lo, &u_hi, &v_lo, &v_hi);
    int same = 0;
    int total = 0;
    for (int i = 1; i < 4; ++i) {
        for (int j = 1; j < 4; ++j) {
            const double u = u_lo + (u_hi - u_lo) * static_cast<double>(i) / 4.0;
            const double v = v_lo + (v_hi - v_lo) * static_cast<double>(j) / 4.0;
            ++total;
            if (tolerance.SamePoint(after.Point(u, v), transform.TransformPoint(before.Point(u, v)))) {
                ++same;
            }
        }
    }
    if (same == total) return Reparameterisation::Same;
    if (same == 0) return Reparameterisation::Reversed;
    return Reparameterisation::Inconsistent;
}

}  // namespace

Mat4d Reflection(const Vec3d &plane_point, const Vec3d &plane_normal) {
    const Vec3d n = plane_normal.Normalized();
    const double d = plane_point.Dot(n);
    Mat4d out;
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            const double ni = i == 0 ? n.x : (i == 1 ? n.y : n.z);
            const double nj = j == 0 ? n.x : (j == 1 ? n.y : n.z);
            out.m[i][j] = (i == j ? 1.0 : 0.0) - 2.0 * ni * nj;
        }
    }
    out.m[0][3] = 2.0 * d * n.x;
    out.m[1][3] = 2.0 * d * n.y;
    out.m[2][3] = 2.0 * d * n.z;
    return out;
}

bool TransformBody(const Model &model, EntityId body, const Mat4d &transform, Model *out,
                   EntityId *out_body, std::string *error) {
    error->clear();
    *out_body = kNoEntity;
    const Body *source = model.GetBody(body);
    if (source == nullptr) {
        *error = "no such body";
        return false;
    }
    const double determinant = transform.LinearDeterminant();
    if (!(std::fabs(determinant) > 1e-12)) {
        *error = "the transform flattens the body";
        return false;
    }
    // A reflection reverses handedness: the surface's own normal, which
    // is the cross product of its two parameter directions, comes out on
    // the far side of the material. Flipping every face's orientation
    // flag puts the outward normals back where they belong. The loops are
    // left exactly as they are -- a loop runs counter-clockwise in its
    // surface's own parameters, and the transform does not touch those.
    const bool mirrored = determinant < 0.0;

    std::map<int, int> curve_map;
    std::map<int, int> surface_map;
    std::map<EntityId, EntityId> vertex_map;
    std::map<EntityId, EntityId> edge_map;

    auto map_curve = [&](int index) {
        auto found = curve_map.find(index);
        if (found != curve_map.end()) return found->second;
        std::unique_ptr<Curve3> copy = model.CurveAt(index)->Clone();
        copy->Transform(transform);
        const int made = out->AddCurve(std::shared_ptr<const Curve3>(copy.release()));
        curve_map[index] = made;
        return made;
    };
    // Each surface is transformed once, and once is also when its
    // reparameterisation is worked out -- see SenseOf above for why that
    // has to be asked per surface and not per body.
    std::map<int, Reparameterisation> sense;
    auto map_surface = [&](int index, std::string *why) {
        auto found = surface_map.find(index);
        if (found != surface_map.end()) return found->second;
        const Surface *before = model.SurfaceAt(index);
        std::unique_ptr<Surface> copy = before->Clone();
        copy->Transform(transform);
        Reparameterisation how = Reparameterisation::Same;
        if (mirrored) {
            how = SenseOf(*before, *copy, transform, Tolerance{});
            if (how == Reparameterisation::Inconsistent) {
                *why = "surface " + std::to_string(index) +
                       " comes out of a reflection carrying its parameters over some of itself and "
                       "not the rest, which this cannot correct for";
            }
        }
        const int made = out->AddSurface(std::shared_ptr<const Surface>(copy.release()));
        surface_map[index] = made;
        sense[index] = how;
        return made;
    };
    auto map_vertex = [&](EntityId id) {
        auto found = vertex_map.find(id);
        if (found != vertex_map.end()) return found->second;
        const Vertex *v = model.GetVertex(id);
        const EntityId made = out->AddVertex(transform.TransformPoint(v->point), v->tolerance);
        vertex_map[id] = made;
        return made;
    };
    auto map_edge = [&](EntityId id) {
        auto found = edge_map.find(id);
        if (found != edge_map.end()) return found->second;
        const Edge *e = model.GetEdge(id);
        const EntityId made =
            out->AddEdge(map_curve(e->curve), map_vertex(e->start_vertex), map_vertex(e->end_vertex),
                         e->t_start, e->t_end, e->tolerance);
        edge_map[id] = made;
        return made;
    };

    std::vector<EntityId> new_shells;
    for (EntityId shell_id : source->shells) {
        const Shell *shell = model.GetShell(shell_id);
        if (shell == nullptr) continue;
        std::vector<EntityId> new_faces;
        for (EntityId face_id : shell->faces) {
            const Face *face = model.GetFace(face_id);
            if (face == nullptr) continue;
            std::string why;
            const int surface = map_surface(face->surface, &why);
            if (!why.empty()) {
                *error = why;
                return false;
            }
            const bool parameters_reversed = sense[face->surface] == Reparameterisation::Reversed;
            std::vector<EntityId> new_loops;
            for (EntityId loop_id : face->loops) {
                const Loop *loop = model.GetLoop(loop_id);
                if (loop == nullptr) continue;
                std::vector<EntityId> coedges;
                for (EntityId coedge_id : loop->coedges) {
                    const CoEdge *coedge = model.GetCoEdge(coedge_id);
                    if (coedge == nullptr) continue;
                    // Turning a loop round means reversing both the order
                    // its edges are visited in and the direction each is
                    // travelled; doing only the first leaves it not
                    // joining up.
                    coedges.push_back(out->AddCoEdge(
                        map_edge(coedge->edge),
                        parameters_reversed ? Flip(coedge->orientation) : coedge->orientation));
                }
                if (parameters_reversed) std::reverse(coedges.begin(), coedges.end());
                new_loops.push_back(out->AddLoop(coedges, loop->is_outer));
            }
            const bool flip_face = mirrored && !parameters_reversed;
            new_faces.push_back(out->AddFace(surface,
                                             flip_face ? Flip(face->orientation) : face->orientation,
                                             new_loops, face->name, face->tolerance));
        }
        new_shells.push_back(out->AddShell(new_faces, shell->is_outer));
    }
    *out_body = out->AddBody(new_shells, source->name);

    PCurveOptions pcurve_options;
    if (!BuildAllPCurves(out, pcurve_options, error)) {
        *error = "the transformed body's p-curves could not be built: " + *error;
        return false;
    }
    return true;
}

bool PatternBody(const Model &model, EntityId body, const std::vector<Mat4d> &placements,
                 PatternCombine combine, const PatternOptions &options, Model *out,
                 std::vector<EntityId> *out_bodies, std::string *error) {
    error->clear();
    *out = Model();
    out_bodies->clear();
    if (placements.empty()) {
        *error = "a pattern needs at least one placement";
        return false;
    }
    if (model.GetBody(body) == nullptr) {
        *error = "no such body";
        return false;
    }

    // Placements that land a copy exactly where an earlier one already
    // is. A circular pattern of something centred on its own axis is the
    // usual way to produce these, and stacking two identical bodies is
    // the coincident-face case the boolean cannot resolve -- so they are
    // dropped here, where the reason is still visible, rather than
    // failing deep inside a classification.
    std::vector<Mat4d> kept;
    for (const Mat4d &placement : placements) {
        bool duplicate = false;
        if (options.skip_coincident) {
            for (const Mat4d &already : kept) {
                double worst = 0.0;
                for (int i = 0; i < 3; ++i) {
                    for (int j = 0; j < 4; ++j) {
                        worst = std::max(worst, std::fabs(placement.m[i][j] - already.m[i][j]));
                    }
                }
                if (worst <= 1e-9) duplicate = true;
            }
        }
        if (!duplicate) kept.push_back(placement);
    }

    if (combine == PatternCombine::Separate) {
        for (std::size_t i = 0; i < kept.size(); ++i) {
            EntityId made = kNoEntity;
            if (!TransformBody(model, body, kept[i], out, &made, error)) {
                *error = "placement " + std::to_string(i + 1) + " of " + std::to_string(kept.size()) +
                         ": " + *error;
                return false;
            }
            out_bodies->push_back(made);
        }
        return true;
    }

    // Merged. Each copy is unioned into what is there already, one at a
    // time, because the boolean takes two bodies -- and doing it in
    // placement order means a failure names which copy could not be
    // merged rather than reporting that the pattern, as a whole, did not
    // work.
    Model current;
    EntityId current_body = kNoEntity;
    if (!TransformBody(model, body, kept.front(), &current, &current_body, error)) return false;
    for (std::size_t i = 1; i < kept.size(); ++i) {
        Model copy;
        EntityId copy_body = kNoEntity;
        if (!TransformBody(model, body, kept[i], &copy, &copy_body, error)) {
            *error = "placement " + std::to_string(i + 1) + ": " + *error;
            return false;
        }
        Model merged;
        BooleanReport report;
        if (!BooleanOperation(current, current_body, copy, copy_body, BooleanOp::Union, &merged, &report,
                              options.boolean)) {
            *error = "copy " + std::to_string(i + 1) + " of " + std::to_string(kept.size()) +
                     " could not be merged: " + report.error;
            return false;
        }
        current = std::move(merged);
        current_body = report.body;
    }
    *out = std::move(current);
    out_bodies->push_back(current_body);
    return true;
}

// --- The generators ----------------------------------------------------

std::vector<Mat4d> LinearPlacements(const Vec3d &direction, double spacing, int count) {
    std::vector<Mat4d> out;
    if (count < 1) return out;
    const double length = direction.Length();
    const Vec3d step = length > 0.0 ? direction * (spacing / length) : Vec3d{};
    for (int i = 0; i < count; ++i) {
        out.push_back(Mat4d::Translation(step * static_cast<double>(i)));
    }
    return out;
}

std::vector<Mat4d> GridPlacements(const Vec3d &direction_u, double spacing_u, int count_u,
                                  const Vec3d &direction_v, double spacing_v, int count_v) {
    std::vector<Mat4d> out;
    for (const Mat4d &v : LinearPlacements(direction_v, spacing_v, count_v)) {
        for (const Mat4d &u : LinearPlacements(direction_u, spacing_u, count_u)) {
            out.push_back(v * u);
        }
    }
    return out;
}

std::vector<Mat4d> CircularPlacements(const Vec3d &axis_point, const Vec3d &axis, int count,
                                      double total_angle, bool close) {
    std::vector<Mat4d> out;
    if (count < 1) return out;
    const double length = axis.Length();
    if (!(length > 0.0)) return out;
    const Vec3d unit = axis * (1.0 / length);
    // `close` spans the angle and leaves the last step open, which is what
    // a full turn wants; otherwise there is a copy at each end.
    const double divisor = close ? static_cast<double>(count) : static_cast<double>(std::max(1, count - 1));
    const Mat4d to_origin = Mat4d::Translation(axis_point * -1.0);
    const Mat4d from_origin = Mat4d::Translation(axis_point);
    for (int i = 0; i < count; ++i) {
        const double angle = total_angle * static_cast<double>(i) / divisor;
        out.push_back(from_origin * Mat4d::Rotation(unit, angle) * to_origin);
    }
    return out;
}

std::vector<Mat4d> MirrorPlacements(const Vec3d &plane_point, const Vec3d &plane_normal) {
    return {Mat4d::Identity(), Reflection(plane_point, plane_normal)};
}

bool SketchPlacements(const Sketch &sketch, const std::vector<SketchId> &points, std::vector<Mat4d> *out,
                      std::string *error) {
    error->clear();
    out->clear();
    if (points.size() < 2) {
        *error = "a sketch-driven pattern needs at least two points: the first says where the seed is "
                 "and the rest say where the copies go";
        return false;
    }
    std::vector<Vec3d> world;
    for (SketchId id : points) {
        const SketchPoint *point = sketch.GetPoint(id);
        if (point == nullptr) {
            *error = "sketch point " + std::to_string(id) + " does not exist";
            return false;
        }
        world.push_back(sketch.Plane().ToWorld(point->position));
    }
    for (const Vec3d &position : world) out->push_back(Mat4d::Translation(position - world.front()));
    return true;
}

}  // namespace cad
