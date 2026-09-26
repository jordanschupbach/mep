#include "cad_modify.h"

#include "cad_pcurve.h"
#include "cad_validate.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace cad {
namespace {


// A plane as the solver wants it: a point and a unit normal. Which way
// the normal points is the *face's* outward direction, not the surface's
// own -- a reversed face's surface normal points into the material, and
// every sign below would be backwards if that were not sorted out once,
// here.
struct FacePlane {
    Vec3d point;
    Vec3d normal;   // outward from the body
    Vec3d x_axis;   // the surface's own axes, kept so that the rebuilt
    Vec3d y_axis;   // surface keeps the orientation the face records
};

bool ReadPlane(const Surface *surface, Orientation orientation, FacePlane *out) {
    const auto *plane = dynamic_cast<const PlaneSurface *>(surface);
    if (plane == nullptr) return false;
    const Vec3d origin = plane->Point(0.0, 0.0);
    out->point = origin;
    out->x_axis = (plane->Point(1.0, 0.0) - origin).Normalized();
    out->y_axis = (plane->Point(0.0, 1.0) - origin).Normalized();
    const Vec3d surface_normal = plane->PlaneNormal().Normalized();
    out->normal = orientation == Orientation::Forward ? surface_normal : surface_normal * -1.0;
    return true;
}

// Every face that touches a vertex, found through the coedges that start
// or end there. A vertex of a polyhedron has three; anything else is
// either a non-manifold point or a shape whose corner is not determined
// by three planes, and both are refused rather than guessed at.
std::vector<EntityId> FacesAtVertex(const Model &model, EntityId shell, EntityId vertex) {
    std::vector<EntityId> faces;
    const Shell *s = model.GetShell(shell);
    if (s == nullptr) return faces;
    for (EntityId face_id : s->faces) {
        const Face *face = model.GetFace(face_id);
        if (face == nullptr) continue;
        bool touches = false;
        for (EntityId loop_id : face->loops) {
            const Loop *loop = model.GetLoop(loop_id);
            if (loop == nullptr) continue;
            for (EntityId coedge_id : loop->coedges) {
                const CoEdge *coedge = model.GetCoEdge(coedge_id);
                const Edge *edge = coedge != nullptr ? model.GetEdge(coedge->edge) : nullptr;
                if (edge == nullptr) continue;
                if (edge->start_vertex == vertex || edge->end_vertex == vertex) touches = true;
            }
        }
        if (touches) faces.push_back(face_id);
    }
    return faces;
}

}  // namespace

bool RebuildOnSurfaces(const Model &model, EntityId body, const std::vector<FaceSurface> &replacements,
                       const ModifyOptions &options, Model *out, EntityId *out_body, std::string *error) {
    error->clear();
    *out = Model();
    *out_body = kNoEntity;

    const Body *source = model.GetBody(body);
    if (source == nullptr) {
        *error = "no such body";
        return false;
    }
    if (source->shells.size() != 1) {
        *error = "this operation handles a body of one shell; a body with a void inside it would need "
                 "its inner shells re-solved too, which is the same routine run again and is not wired "
                 "up";
        return false;
    }
    const EntityId shell_id = source->shells.front();
    const Shell *shell = model.GetShell(shell_id);
    if (shell == nullptr) {
        *error = "the body's shell is missing";
        return false;
    }

    // --- The planes, old and new ---------------------------------------
    std::map<EntityId, FacePlane> planes;
    for (EntityId face_id : shell->faces) {
        const Face *face = model.GetFace(face_id);
        if (face == nullptr) continue;
        FacePlane plane;
        if (!ReadPlane(model.SurfaceAt(face->surface), face->orientation, &plane)) {
            *error = "face " + std::to_string(face_id) +
                     " is not a plane; this operation re-solves each corner as the meeting of three "
                     "planes, and a curved face needs its edges intersected instead";
            return false;
        }
        planes[face_id] = plane;
    }
    for (const FaceSurface &replacement : replacements) {
        const auto found = planes.find(replacement.face);
        if (found == planes.end()) {
            *error = "a replacement names face " + std::to_string(replacement.face) +
                     ", which is not in this body";
            return false;
        }
        const Face *face = model.GetFace(replacement.face);
        FacePlane plane;
        if (!ReadPlane(replacement.surface.get(), face->orientation, &plane)) {
            *error = "the replacement for face " + std::to_string(replacement.face) + " is not a plane";
            return false;
        }
        found->second = plane;
    }

    // --- The corners ----------------------------------------------------
    //
    // A corner is wherever its three faces now meet, which is one 3x3
    // solve. Three planes fail to meet in a point exactly when two of
    // them are parallel or all three share a direction, and that is a
    // real answer about the shape rather than a numerical accident, so it
    // is reported as what it is.
    const std::vector<EntityId> vertices = model.VerticesOfShell(shell_id);
    std::map<EntityId, Vec3d> moved;
    for (EntityId vertex_id : vertices) {
        const std::vector<EntityId> at = FacesAtVertex(model, shell_id, vertex_id);
        if (at.size() != 3) {
            *error = "vertex " + std::to_string(vertex_id) + " has " + std::to_string(at.size()) +
                     " faces meeting at it; this operation needs exactly three, since that is what "
                     "fixes a corner once the faces have moved";
            return false;
        }
        const FacePlane &a = planes[at[0]];
        const FacePlane &b = planes[at[1]];
        const FacePlane &c = planes[at[2]];
        Vec3d point;
        if (!Mat3d::FromRows(a.normal, b.normal, c.normal)
                 .Solve(Vec3d{a.normal.Dot(a.point), b.normal.Dot(b.point), c.normal.Dot(c.point)},
                        &point)) {
            *error = "the three faces at vertex " + std::to_string(vertex_id) +
                     " no longer meet at a point";
            return false;
        }
        moved[vertex_id] = point;
    }

    // --- Did anything turn inside out? ----------------------------------
    //
    // This is the self-intersection test, and it is exact. An edge is
    // between the same two corners it always was; if the vector from one
    // to the other has reversed, those corners have swapped over, which
    // is what it looks like locally when an offset has eaten the feature
    // they belonged to. Checking it here means the refusal can name the
    // edge instead of the caller discovering later that the volume came
    // out negative.
    const std::vector<EntityId> edges = model.EdgesOfShell(shell_id);
    for (EntityId edge_id : edges) {
        const Edge *edge = model.GetEdge(edge_id);
        if (edge == nullptr) continue;
        if (edge->IsClosed()) {
            *error = "edge " + std::to_string(edge_id) +
                     " is closed, so it has no two corners to re-solve; a body with a seam needs the "
                     "curved-face handling this operation does not have";
            return false;
        }
        const Vec3d before = model.EdgeEndPoint(edge_id) - model.EdgeStartPoint(edge_id);
        const Vec3d after = moved[edge->end_vertex] - moved[edge->start_vertex];
        if (!(after.Length() > 0.0)) {
            *error = "edge " + std::to_string(edge_id) + " collapsed to nothing";
            return false;
        }
        if (before.Dot(after) <= 0.0) {
            *error = "edge " + std::to_string(edge_id) +
                     " turned back on itself, so the faces around it have passed through each other; "
                     "the change asked for is larger than this shape has room for";
            return false;
        }
    }

    // --- Emit -----------------------------------------------------------
    //
    // The planes are rebuilt here rather than above because only now is
    // it known where the face's corners ended up, and a surface trimmed
    // to less than its face is the surest way to lose a p-curve.
    std::map<EntityId, EntityId> new_vertex;
    for (EntityId vertex_id : vertices) new_vertex[vertex_id] = out->AddVertex(moved[vertex_id]);
    std::map<EntityId, EntityId> new_edge;
    for (EntityId edge_id : edges) {
        const Edge *edge = model.GetEdge(edge_id);
        const int curve = out->AddCurve(std::make_shared<Line3>(
            Line3::FromPoints(moved[edge->start_vertex], moved[edge->end_vertex])));
        new_edge[edge_id] =
            out->AddEdge(curve, new_vertex[edge->start_vertex], new_vertex[edge->end_vertex], 0.0, 1.0);
    }

    std::vector<EntityId> new_faces;
    for (EntityId face_id : shell->faces) {
        const Face *face = model.GetFace(face_id);
        if (face == nullptr) continue;
        const FacePlane &plane = planes[face_id];
        Interval u;
        Interval v;
        for (EntityId loop_id : face->loops) {
            const Loop *loop = model.GetLoop(loop_id);
            if (loop == nullptr) continue;
            for (EntityId coedge_id : loop->coedges) {
                const CoEdge *coedge = model.GetCoEdge(coedge_id);
                const Edge *edge = coedge != nullptr ? model.GetEdge(coedge->edge) : nullptr;
                if (edge == nullptr) continue;
                for (EntityId end : {edge->start_vertex, edge->end_vertex}) {
                    const Vec3d relative = moved[end] - plane.point;
                    u.Expand(relative.Dot(plane.x_axis));
                    v.Expand(relative.Dot(plane.y_axis));
                }
            }
        }
        const double pad = std::max(1e-6, std::max(u.Width(), v.Width()) * 1e-6);
        const int surface = out->AddSurface(std::make_shared<PlaneSurface>(
            plane.point, plane.x_axis, plane.y_axis, u.lo - pad, u.hi + pad, v.lo - pad, v.hi + pad));
        std::vector<EntityId> new_loops;
        for (EntityId loop_id : face->loops) {
            const Loop *loop = model.GetLoop(loop_id);
            if (loop == nullptr) continue;
            std::vector<EntityId> coedges;
            for (EntityId coedge_id : loop->coedges) {
                const CoEdge *coedge = model.GetCoEdge(coedge_id);
                if (coedge == nullptr) continue;
                coedges.push_back(out->AddCoEdge(new_edge[coedge->edge], coedge->orientation));
            }
            new_loops.push_back(out->AddLoop(coedges, loop->is_outer));
        }
        new_faces.push_back(
            out->AddFace(surface, face->orientation, new_loops, face->name, face->tolerance));
    }
    *out_body = out->AddBody({out->AddShell(new_faces, true)}, source->name);

    PCurveOptions pcurve_options;
    pcurve_options.tolerance = options.tolerance;
    if (!BuildAllPCurves(out, pcurve_options, error)) {
        *error = "the re-solved body's p-curves could not be built: " + *error;
        return false;
    }
    return true;
}

namespace {

// A plane moved along its own outward normal, with the surface's axes
// left alone so that the face's Forward/Reversed flag still means what
// it meant.
std::shared_ptr<const Surface> MovedPlane(const FacePlane &plane, double distance) {
    return std::make_shared<PlaneSurface>(plane.point + plane.normal * distance, plane.x_axis,
                                          plane.y_axis);
}

bool PlaneOfFace(const Model &model, EntityId face_id, FacePlane *out, std::string *error) {
    const Face *face = model.GetFace(face_id);
    if (face == nullptr) {
        *error = "no such face: " + std::to_string(face_id);
        return false;
    }
    if (!ReadPlane(model.SurfaceAt(face->surface), face->orientation, out)) {
        *error = "face " + std::to_string(face_id) + " is not a plane";
        return false;
    }
    return true;
}

std::vector<EntityId> FacesOfBody(const Model &model, EntityId body) {
    std::vector<EntityId> faces;
    const Body *solid = model.GetBody(body);
    if (solid == nullptr) return faces;
    for (EntityId shell_id : solid->shells) {
        const Shell *shell = model.GetShell(shell_id);
        if (shell == nullptr) continue;
        for (EntityId face : shell->faces) faces.push_back(face);
    }
    return faces;
}

}  // namespace

bool OffsetBody(const Model &model, EntityId body, double distance, const ModifyOptions &options,
                Model *out, EntityId *out_body, std::string *error) {
    error->clear();
    if (!(std::fabs(distance) > 0.0)) {
        *error = "an offset of zero has nothing to do";
        return false;
    }
    std::vector<FaceSurface> replacements;
    for (EntityId face_id : FacesOfBody(model, body)) {
        FacePlane plane;
        if (!PlaneOfFace(model, face_id, &plane, error)) return false;
        replacements.push_back(FaceSurface{face_id, MovedPlane(plane, distance)});
    }
    return RebuildOnSurfaces(model, body, replacements, options, out, out_body, error);
}

bool DraftFaces(const Model &model, EntityId body, const std::vector<EntityId> &faces,
                const Vec3d &neutral_point, const Vec3d &pull_direction, double angle,
                const ModifyOptions &options, Model *out, EntityId *out_body, std::string *error) {
    error->clear();
    if (faces.empty()) {
        *error = "a draft needs at least one face";
        return false;
    }
    const double pull_length = pull_direction.Length();
    if (!(pull_length > 0.0)) {
        *error = "the pull direction has no direction";
        return false;
    }
    const Vec3d pull = pull_direction * (1.0 / pull_length);

    std::vector<FaceSurface> replacements;
    for (EntityId face_id : faces) {
        FacePlane plane;
        if (!PlaneOfFace(model, face_id, &plane, error)) return false;
        // The hinge: the line where this face meets the neutral plane.
        // The face turns about it, so every point of that line stays put
        // and the part of the body at the neutral plane keeps its size --
        // which is what "neutral" means and why a draft is specified
        // against one.
        const Vec3d hinge = plane.normal.Cross(pull);
        const double hinge_length = hinge.Length();
        if (!(hinge_length > 1e-9)) {
            *error = "face " + std::to_string(face_id) +
                     " is square to the pull direction, so it has no hinge to turn about; a face "
                     "facing the way the mould opens cannot be drafted";
            return false;
        }
        const Vec3d axis = hinge * (1.0 / hinge_length);
        Vec3d anchor;
        if (!Mat3d::FromRows(plane.normal, pull, axis)
                 .Solve(Vec3d{plane.normal.Dot(plane.point), pull.Dot(neutral_point), 0.0}, &anchor)) {
            *error = "the hinge line for face " + std::to_string(face_id) + " is not determined";
            return false;
        }
        // Rodrigues about the hinge. Turning the outward normal towards
        // the pull direction is what leans the face inwards as the body
        // rises, so a positive angle narrows the part going up -- the
        // sign a mould tool means.
        const Vec3d turned = plane.normal * std::cos(angle) +
                             axis.Cross(plane.normal) * std::sin(angle) +
                             axis * (axis.Dot(plane.normal) * (1.0 - std::cos(angle)));
        const Face *face = model.GetFace(face_id);
        const Vec3d surface_normal =
            face->orientation == Orientation::Forward ? turned : turned * -1.0;
        // Axes for the new plane, chosen so their cross product is the
        // surface's normal and not the face's -- the face keeps the
        // orientation flag it had.
        // x cross y has to come out as the *surface's* normal, since
        // that is what PlaneSurface reports and what the face's
        // orientation flag is recorded against.
        const Vec3d x = axis;
        const Vec3d y = surface_normal.Cross(x).Normalized();
        replacements.push_back(
            FaceSurface{face_id, std::make_shared<PlaneSurface>(anchor, x, y)});
    }
    return RebuildOnSurfaces(model, body, replacements, options, out, out_body, error);
}

bool ShellBody(const Model &model, EntityId body, const std::vector<EntityId> &open_faces,
               double thickness, const ModifyOptions &options, Model *out, EntityId *out_body,
               std::string *error) {
    error->clear();
    *out = Model();
    *out_body = kNoEntity;
    if (!(thickness > 0.0)) {
        *error = "a shell needs a positive wall thickness";
        return false;
    }
    const std::vector<EntityId> faces = FacesOfBody(model, body);
    for (EntityId open : open_faces) {
        if (std::find(faces.begin(), faces.end(), open) == faces.end()) {
            *error = "face " + std::to_string(open) + " is not in this body, so it cannot be opened";
            return false;
        }
    }

    // The cavity. Faces that stay move inwards by the wall thickness;
    // faces that are opened stay exactly where they are, so that the
    // cavity runs out to them rather than stopping a wall short. That one
    // distinction is the whole of shelling.
    std::vector<FaceSurface> replacements;
    for (EntityId face_id : faces) {
        if (std::find(open_faces.begin(), open_faces.end(), face_id) != open_faces.end()) continue;
        FacePlane plane;
        if (!PlaneOfFace(model, face_id, &plane, error)) return false;
        replacements.push_back(FaceSurface{face_id, MovedPlane(plane, -thickness)});
    }
    Model cavity;
    EntityId cavity_body = kNoEntity;
    if (!RebuildOnSurfaces(model, body, replacements, options, &cavity, &cavity_body, error)) {
        *error = "the cavity could not be formed: " + *error;
        return false;
    }
    // The faces of the two bodies line up one for one, because the cavity
    // was re-solved on the same topology. That correspondence is what
    // lets an opened face be found again on the inside.
    const std::vector<EntityId> cavity_faces = FacesOfBody(cavity, cavity_body);
    if (cavity_faces.size() != faces.size()) {
        *error = "the cavity did not come out with the same faces as the body";
        return false;
    }

    // --- Assemble -------------------------------------------------------
    std::map<EntityId, EntityId> outer_vertex;
    std::map<EntityId, EntityId> outer_edge;
    std::map<EntityId, EntityId> inner_vertex;
    std::map<EntityId, EntityId> inner_edge;

    auto copy_vertex = [&](const Model &from, EntityId id, std::map<EntityId, EntityId> *into) {
        auto found = into->find(id);
        if (found != into->end()) return found->second;
        const Vertex *v = from.GetVertex(id);
        const EntityId made = out->AddVertex(v->point, v->tolerance);
        (*into)[id] = made;
        return made;
    };
    auto copy_edge = [&](const Model &from, EntityId id, std::map<EntityId, EntityId> *edges,
                         std::map<EntityId, EntityId> *vertices) {
        auto found = edges->find(id);
        if (found != edges->end()) return found->second;
        const Edge *e = from.GetEdge(id);
        const int curve = out->AddCurve(std::shared_ptr<const Curve3>(from.CurveAt(e->curve)->Clone().release()));
        const EntityId made =
            out->AddEdge(curve, copy_vertex(from, e->start_vertex, vertices),
                         copy_vertex(from, e->end_vertex, vertices), e->t_start, e->t_end, e->tolerance);
        (*edges)[id] = made;
        return made;
    };
    // `flip` turns a face round: the cavity's faces bound the body from
    // the inside, so every one of them faces the other way.
    //
    // Only the orientation flag changes. A loop runs counter-clockwise in
    // its surface's own (u, v) whichever way the face points -- that is
    // the convention the whole kernel builds to and the validator checks
    // -- and the coedges along a shared edge already run opposite ways
    // within the cavity, so leaving them alone is what keeps them
    // opposite here.
    auto copy_face = [&](const Model &from, EntityId id, bool flip, std::map<EntityId, EntityId> *edges,
                         std::map<EntityId, EntityId> *vertices, const std::string &name) {
        const Face *face = from.GetFace(id);
        const int surface =
            out->AddSurface(std::shared_ptr<const Surface>(from.SurfaceAt(face->surface)->Clone().release()));
        std::vector<EntityId> loops;
        for (EntityId loop_id : face->loops) {
            const Loop *loop = from.GetLoop(loop_id);
            std::vector<EntityId> coedges;
            for (EntityId coedge_id : loop->coedges) {
                const CoEdge *coedge = from.GetCoEdge(coedge_id);
                coedges.push_back(out->AddCoEdge(copy_edge(from, coedge->edge, edges, vertices),
                                                 coedge->orientation));
            }
            loops.push_back(out->AddLoop(coedges, loop->is_outer));
        }
        return out->AddFace(surface, flip ? Flip(face->orientation) : face->orientation, loops,
                            name.empty() ? face->name : name, face->tolerance);
    };

    std::vector<EntityId> outer_shell_faces;
    std::vector<EntityId> cavity_shell_faces;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        const bool opened =
            std::find(open_faces.begin(), open_faces.end(), faces[i]) != open_faces.end();
        if (opened) continue;
        outer_shell_faces.push_back(copy_face(model, faces[i], false, &outer_edge, &outer_vertex, ""));
        cavity_shell_faces.push_back(
            copy_face(cavity, cavity_faces[i], true, &inner_edge, &inner_vertex, "inner"));
    }

    if (open_faces.empty()) {
        // Nothing was opened, so the cavity is sealed and belongs to the
        // body as a void -- an inner shell, which is exactly the thing
        // Part B.1 introduced them for.
        *out_body = out->AddBody({out->AddShell(outer_shell_faces, true),
                                  out->AddShell(cavity_shell_faces, false)},
                                 model.GetBody(body)->name);
    } else {
        // Each opened face becomes a rim: the face as it was, with the
        // cavity's face in the same plane as a hole through it.
        for (std::size_t i = 0; i < faces.size(); ++i) {
            if (std::find(open_faces.begin(), open_faces.end(), faces[i]) == open_faces.end()) continue;
            const Face *face = model.GetFace(faces[i]);
            const Face *mouth = cavity.GetFace(cavity_faces[i]);
            const int surface = out->AddSurface(
                std::shared_ptr<const Surface>(model.SurfaceAt(face->surface)->Clone().release()));
            std::vector<EntityId> loops;
            for (EntityId loop_id : face->loops) {
                const Loop *loop = model.GetLoop(loop_id);
                std::vector<EntityId> coedges;
                for (EntityId coedge_id : loop->coedges) {
                    const CoEdge *coedge = model.GetCoEdge(coedge_id);
                    coedges.push_back(out->AddCoEdge(
                        copy_edge(model, coedge->edge, &outer_edge, &outer_vertex), coedge->orientation));
                }
                loops.push_back(out->AddLoop(coedges, loop->is_outer));
            }
            // The mouth, reversed twice over: once because the cavity's
            // face pointed into the material, and once because a hole in
            // a face runs opposite to the boundary around it.
            for (EntityId loop_id : mouth->loops) {
                const Loop *loop = cavity.GetLoop(loop_id);
                std::vector<EntityId> coedges;
                for (EntityId coedge_id : loop->coedges) {
                    const CoEdge *coedge = cavity.GetCoEdge(coedge_id);
                    coedges.push_back(out->AddCoEdge(
                        copy_edge(cavity, coedge->edge, &inner_edge, &inner_vertex),
                        Flip(coedge->orientation)));
                }
                std::reverse(coedges.begin(), coedges.end());
                loops.push_back(out->AddLoop(coedges, false));
            }
            outer_shell_faces.push_back(
                out->AddFace(surface, face->orientation, loops, "rim", face->tolerance));
        }
        std::vector<EntityId> all = outer_shell_faces;
        for (EntityId f : cavity_shell_faces) all.push_back(f);
        *out_body = out->AddBody({out->AddShell(all, true)}, model.GetBody(body)->name);
    }

    PCurveOptions pcurve_options;
    pcurve_options.tolerance = options.tolerance;
    if (!BuildAllPCurves(out, pcurve_options, error)) {
        *error = "the shelled body's p-curves could not be built: " + *error;
        return false;
    }
    return true;
}

}  // namespace cad
