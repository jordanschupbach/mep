// Curved second-order elements (plans/CAD_FEM_PLAN.md Part G.5).
//
// A first-order tetrahedron has flat faces, so a mesh of them
// approximates a curved boundary by chords and the error in the boundary
// is the error in the answer, however fine the elements. A second-order
// one carries a node at the middle of each edge and its faces are
// quadratic surfaces, which can follow a curve -- but only if the
// mid-node is put where the curve is rather than halfway between its
// ends, and only the exact geometry knows where that is.
//
// THIS IS THE CONCRETE PAYOFF of the whole B-rep half of this plan. A
// pipeline that meshes a tessellation cannot do this at all: by the time
// it sees the model the curve is already chords, and the middle of a
// chord is exactly where the mid-node should not go.
//
// AND THE VALIDITY CHECK IS WHAT MAKES IT USABLE RATHER THAN MERELY
// CLEVER. Pulling a mid-node onto a tight fillet can turn its element
// inside out -- the quadratic map's Jacobian goes negative somewhere
// inside an element whose corners are all in the right places -- and an
// inverted element is worse than a straight one, because a straight one
// is merely inaccurate while an inverted one makes the assembled system
// wrong. Where that happens the node is walked back towards the straight
// position until every element holding it is valid again, and how often
// that was needed is reported rather than hidden.

#include "fem_mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fem {
namespace {

using cad::Vec3d;

// THE TEN-NODE ORDERING, ONCE, EXPLICITLY. Corners first, then the
// mid-edge nodes in the order used by Abaqus's C3D10 and by VTK's
// quadratic tetrahedron, which is the one every mesh format and every
// element library this will meet expects:
//
//   0..3  the corners a, b, c, d
//   4     the middle of edge 0-1      7     the middle of edge 0-3
//   5     the middle of edge 1-2      8     the middle of edge 1-3
//   6     the middle of edge 0-2      9     the middle of edge 2-3
//
// Getting this wrong produces a mesh that looks right, measures right and
// solves to nonsense, so it is written down rather than remembered.
constexpr int kEdgeEnds[6][2] = {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}};
constexpr int kEdgeSlot[6] = {4, 5, 6, 7, 8, 9};

// The quadratic tetrahedron's shape functions in barycentric form, and
// their derivatives with respect to the three reference coordinates.
// L0 = 1-r-s-t, L1 = r, L2 = s, L3 = t; a corner's function is
// L(2L-1) and a mid-edge node's is 4 L_i L_j.
void ShapeDerivatives(double r, double s, double t, double d[10][3]) {
    const double barycentric[4] = {1.0 - r - s - t, r, s, t};
    // Derivatives of the barycentric coordinates themselves.
    const double dL[4][3] = {{-1.0, -1.0, -1.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}};
    for (int corner = 0; corner < 4; ++corner) {
        for (int axis = 0; axis < 3; ++axis) {
            d[corner][axis] = (4.0 * barycentric[corner] - 1.0) * dL[corner][axis];
        }
    }
    for (int edge = 0; edge < 6; ++edge) {
        const int i = kEdgeEnds[edge][0];
        const int j = kEdgeEnds[edge][1];
        for (int axis = 0; axis < 3; ++axis) {
            d[kEdgeSlot[edge]][axis] =
                4.0 * (dL[i][axis] * barycentric[j] + barycentric[i] * dL[j][axis]);
        }
    }
}

double JacobianAt(const std::vector<Vec3d> &nodes, const std::array<int, 10> &element, double r,
                  double s, double t) {
    double d[10][3];
    ShapeDerivatives(r, s, t, d);
    Vec3d column[3] = {Vec3d{}, Vec3d{}, Vec3d{}};
    for (int i = 0; i < 10; ++i) {
        const Vec3d &p = nodes[static_cast<std::size_t>(element[static_cast<std::size_t>(i)])];
        for (int axis = 0; axis < 3; ++axis) {
            column[axis] = column[axis] + p * d[i][axis];
        }
    }
    return column[0].Cross(column[1]).Dot(column[2]);
}

// Positive everywhere it is asked. THE CORNERS ARE NOT ENOUGH: a
// quadratic map can fold in the middle of an element whose corners are
// all fine, which is exactly what an over-pulled mid-node does. So the
// corners, the centroid and the six mid-edge points are all checked --
// the standard sample set, and the mid-edge points are where the fold
// shows first.
bool ElementIsValid(const std::vector<Vec3d> &nodes, const std::array<int, 10> &element) {
    static const double samples[11][3] = {
        {0.0, 0.0, 0.0}, {1.0, 0.0, 0.0}, {0.0, 1.0, 0.0}, {0.0, 0.0, 1.0}, {0.25, 0.25, 0.25},
        {0.5, 0.0, 0.0}, {0.5, 0.5, 0.0}, {0.0, 0.5, 0.0}, {0.0, 0.0, 0.5}, {0.5, 0.0, 0.5},
        {0.0, 0.5, 0.5}};
    for (const auto &at : samples) {
        if (!(JacobianAt(nodes, element, at[0], at[1], at[2]) > 0.0)) return false;
    }
    return true;
}

std::array<int, 2> Key(int a, int b) {
    return a < b ? std::array<int, 2>{a, b} : std::array<int, 2>{b, a};
}

// Where a node sits on the model: the CAD edges it lies on and the CAD
// faces it lies on. A node recorded against an edge lies on both faces
// that meet there, and one recorded against a vertex lies on every edge
// and face around it -- which is what makes a mid-node at a corner come
// out right instead of being left straight.
struct OnModel {
    std::vector<cad::EntityId> edges;
    std::vector<cad::EntityId> faces;
};

std::vector<cad::EntityId> Shared(const std::vector<cad::EntityId> &a,
                                  const std::vector<cad::EntityId> &b) {
    std::vector<cad::EntityId> out;
    for (const cad::EntityId id : a) {
        if (std::find(b.begin(), b.end(), id) != b.end()) out.push_back(id);
    }
    return out;
}

}  // namespace

bool MakeSecondOrder(const cad::Model &model, VolumeMesh *mesh, MeshReport *report) {
    if (mesh == nullptr || report == nullptr) return false;
    mesh->tets10.clear();
    mesh->boundary6.clear();
    report->mid_nodes = 0;
    report->mid_nodes_curved = 0;
    report->mid_nodes_backed_off = 0;
    if (mesh->tets.empty()) {
        report->error = "the mesh has no elements to raise the order of";
        return false;
    }
    if (mesh->provenance.size() != mesh->nodes.size()) {
        report->error = "the mesh's provenance does not cover its nodes, so its boundary cannot "
                        "be put back on the model";
        return false;
    }

    // Which edges and faces each node lies on.
    std::map<cad::EntityId, std::vector<cad::EntityId>> edges_of_vertex;
    for (const cad::Edge &edge : model.Edges()) {
        edges_of_vertex[edge.start_vertex].push_back(edge.id);
        if (edge.end_vertex != edge.start_vertex) edges_of_vertex[edge.end_vertex].push_back(edge.id);
    }
    std::vector<OnModel> where(mesh->nodes.size());
    for (std::size_t i = 0; i < mesh->nodes.size(); ++i) {
        const SurfaceMesh::Provenance &from = mesh->provenance[i];
        if (from.face != cad::kNoEntity) {
            where[i].faces.push_back(from.face);
        } else if (from.edge != cad::kNoEntity) {
            where[i].edges.push_back(from.edge);
            where[i].faces = model.FacesOfEdge(from.edge);
        } else if (from.vertex != cad::kNoEntity) {
            const auto found = edges_of_vertex.find(from.vertex);
            if (found != edges_of_vertex.end()) {
                where[i].edges = found->second;
                for (const cad::EntityId edge : found->second) {
                    for (const cad::EntityId face : model.FacesOfEdge(edge)) {
                        if (std::find(where[i].faces.begin(), where[i].faces.end(), face) ==
                            where[i].faces.end()) {
                            where[i].faces.push_back(face);
                        }
                    }
                }
            }
        }
    }

    // The parameter of a node along one of the CAD edges it lies on.
    auto parameter_on = [&](int node, cad::EntityId edge_id, double *out) {
        const SurfaceMesh::Provenance &from = mesh->provenance[static_cast<std::size_t>(node)];
        const cad::Edge *edge = model.GetEdge(edge_id);
        if (edge == nullptr) return false;
        if (from.edge == edge_id) {
            *out = from.u;
            return true;
        }
        if (from.vertex == cad::kNoEntity) return false;
        // A closed edge starts and ends at the same vertex, so the
        // parameter there is genuinely ambiguous and is left alone rather
        // than guessed at.
        if (edge->start_vertex == edge->end_vertex) return false;
        if (from.vertex == edge->start_vertex) {
            *out = edge->t_start;
            return true;
        }
        if (from.vertex == edge->end_vertex) {
            *out = edge->t_end;
            return true;
        }
        return false;
    };

    // ONLY AN EDGE OF THE BOUNDARY MAY BE CURVED. Two nodes lying on the
    // same CAD face does not make the edge between them an edge of that
    // face: a chord straight through the middle of a sphere has both
    // ends on it, and pulling that chord's midpoint out to the surface
    // would turn an element inside out for no reason at all. The edges
    // that are genuinely on the surface are exactly the edges of the
    // boundary triangles, so that is what is asked.
    std::set<std::array<int, 2>> boundary_edges;
    for (const SurfaceTriangle &face : mesh->boundary) {
        boundary_edges.insert(Key(face.a, face.b));
        boundary_edges.insert(Key(face.b, face.c));
        boundary_edges.insert(Key(face.c, face.a));
    }

    // Every mesh edge gets one mid-node, made once and shared.
    struct MidNode {
        int index = 0;
        Vec3d straight;
        Vec3d curved;
        bool moved = false;   // the geometry had something to say about it
        double pulled = 1.0;  // how far along the way from straight to curved
    };
    std::map<std::array<int, 2>, MidNode> middles;
    auto mid_node_for = [&](int a, int b) {
        const std::array<int, 2> key = Key(a, b);
        const auto found = middles.find(key);
        if (found != middles.end()) return found->second.index;
        const Vec3d &pa = mesh->nodes[static_cast<std::size_t>(a)];
        const Vec3d &pb = mesh->nodes[static_cast<std::size_t>(b)];
        MidNode made;
        made.straight = (pa + pb) * 0.5;
        made.curved = made.straight;
        // THE GUARD ON EVERY PROJECTION. A mid-node belongs near the
        // middle of its own edge; anything further is the projection
        // having found some other part of the surface -- the far side of
        // a cylinder across its seam, or the wrong one of two arcs
        // between the same pair of vertices. Half the edge's length is
        // far more than any real sagitta and far less than any such
        // mistake.
        const double reach = (pb - pa).Length() * 0.5;
        // "Moved" has to mean moved. Projecting the middle of an edge of
        // a planar face onto that plane returns it where it already was,
        // give or take the last bit of the mantissa, and counting those
        // as curved would report a box as having 312 curved nodes and
        // hide whether a sphere had any.
        const double meaningful = reach * 2e-9;
        if (boundary_edges.count(key) == 0) {
            made.index = mesh->NodeCount();
            mesh->nodes.push_back(made.curved);
            mesh->provenance.push_back({});
            ++report->mid_nodes;
            middles[key] = made;
            return made.index;
        }

        // On a CAD edge: the parametric middle, which needs no search and
        // cannot land on the wrong branch of anything.
        for (const cad::EntityId edge : Shared(where[static_cast<std::size_t>(a)].edges,
                                               where[static_cast<std::size_t>(b)].edges)) {
            double ta = 0.0;
            double tb = 0.0;
            if (!parameter_on(a, edge, &ta) || !parameter_on(b, edge, &tb)) continue;
            if (ta == tb) continue;
            const Vec3d at = model.EdgePoint(edge, (ta + tb) * 0.5);
            const double moved_by = (at - made.straight).Length();
            if (moved_by > reach) continue;
            if (moved_by <= meaningful) break;
            made.curved = at;
            made.moved = true;
            break;
        }
        // On a CAD face, if it was not on an edge: the closest point of
        // the surface, seeded from the straight middle.
        if (!made.moved) {
            for (const cad::EntityId face_id : Shared(where[static_cast<std::size_t>(a)].faces,
                                                      where[static_cast<std::size_t>(b)].faces)) {
                const cad::Face *face = model.GetFace(face_id);
                if (face == nullptr) continue;
                const cad::Surface *surface = model.SurfaceAt(face->surface);
                if (surface == nullptr) continue;
                double u = 0.0;
                double v = 0.0;
                Vec3d at;
                if (!surface->ClosestPoint(made.straight, &u, &v, &at, {})) continue;
                const double moved_by = (at - made.straight).Length();
                if (moved_by > reach) continue;
                if (moved_by <= meaningful) break;
                made.curved = at;
                made.moved = true;
                break;
            }
        }
        made.index = mesh->NodeCount();
        mesh->nodes.push_back(made.curved);
        mesh->provenance.push_back({});
        ++report->mid_nodes;
        if (made.moved) ++report->mid_nodes_curved;
        middles[key] = made;
        return made.index;
    };

    mesh->tets10.reserve(mesh->tets.size());
    for (const Tetrahedron &t : mesh->tets) {
        const int corner[4] = {t.a, t.b, t.c, t.d};
        std::array<int, 10> element{};
        for (int i = 0; i < 4; ++i) element[static_cast<std::size_t>(i)] = corner[i];
        for (int edge = 0; edge < 6; ++edge) {
            element[static_cast<std::size_t>(kEdgeSlot[edge])] =
                mid_node_for(corner[kEdgeEnds[edge][0]], corner[kEdgeEnds[edge][1]]);
        }
        mesh->tets10.push_back(element);
    }

    // --- Back off until every element is valid -----------------------------
    //
    // A mid-node is shared, so pulling one back can fix one element and
    // is not allowed to have broken another; the only honest way to do it
    // is to keep sweeping until nothing is invalid. Halving converges in
    // a handful of sweeps and ends at the straight position, which is
    // always valid because the first-order mesh it came from was checked.
    std::map<int, std::array<int, 2>> key_of_node;
    for (const auto &entry : middles) key_of_node[entry.second.index] = entry.first;

    std::set<int> pulled_back;
    for (int sweep = 0; sweep < 24; ++sweep) {
        bool any = false;
        for (const std::array<int, 10> &element : mesh->tets10) {
            if (ElementIsValid(mesh->nodes, element)) continue;
            any = true;
            for (int slot = 4; slot < 10; ++slot) {
                const int node = element[static_cast<std::size_t>(slot)];
                const auto found = key_of_node.find(node);
                if (found == key_of_node.end()) continue;
                MidNode &mid = middles[found->second];
                if (mid.pulled <= 0.0) continue;
                mid.pulled = mid.pulled > 0.03 ? mid.pulled * 0.5 : 0.0;
                mesh->nodes[static_cast<std::size_t>(node)] =
                    mid.straight + (mid.curved - mid.straight) * mid.pulled;
                pulled_back.insert(node);
            }
        }
        if (!any) break;
    }
    report->mid_nodes_backed_off = static_cast<int>(pulled_back.size());
    for (const int node : pulled_back) {
        const MidNode &mid = middles[key_of_node[node]];
        if (mid.pulled <= 0.0 && mid.moved) --report->mid_nodes_curved;
    }

    for (const std::array<int, 10> &element : mesh->tets10) {
        if (ElementIsValid(mesh->nodes, element)) continue;
        report->error = "an element could not be made valid even with its mid-side nodes put back "
                        "where the straight mesh had them, which means the first-order mesh it "
                        "came from was already inverted";
        return false;
    }

    // The boundary as six-node triangles, so that a pressure or a contact
    // on a curved face is applied to the curve and not to its chords.
    mesh->boundary6.reserve(mesh->boundary.size());
    for (const SurfaceTriangle &face : mesh->boundary) {
        std::array<int, 6> six{};
        six[0] = face.a;
        six[1] = face.b;
        six[2] = face.c;
        const std::array<int, 2> ab = Key(face.a, face.b);
        const std::array<int, 2> bc = Key(face.b, face.c);
        const std::array<int, 2> ca = Key(face.c, face.a);
        const auto find = [&](const std::array<int, 2> &key) {
            const auto found = middles.find(key);
            return found == middles.end() ? -1 : found->second.index;
        };
        six[3] = find(ab);
        six[4] = find(bc);
        six[5] = find(ca);
        if (six[3] < 0 || six[4] < 0 || six[5] < 0) {
            report->error = "a boundary triangle is not a face of any element, so it has no "
                            "mid-side nodes";
            return false;
        }
        mesh->boundary6.push_back(six);
    }

    report->nodes = mesh->NodeCount();
    return true;
}

bool CheckCurvature(const VolumeMesh &mesh, std::string *error) {
    for (std::size_t i = 0; i < mesh.tets10.size(); ++i) {
        for (const int node : mesh.tets10[i]) {
            if (node >= 0 && node < mesh.NodeCount()) continue;
            *error = "second-order element " + std::to_string(i) + " refers to node " +
                     std::to_string(node) + ", which does not exist";
            return false;
        }
        if (ElementIsValid(mesh.nodes, mesh.tets10[i])) continue;
        *error = "second-order element " + std::to_string(i) +
                 " is turned inside out somewhere within itself; its corners may well be fine, "
                 "which is why this is asked of the map and not of them";
        return false;
    }
    return true;
}

double CurvedVolume(const VolumeMesh &mesh) {
    // Five points, exact to cubic. The map is quadratic, so its
    // derivatives are linear and the determinant of three of them is
    // cubic -- so this rule is not an approximation of the integral, it
    // is the integral. A finer rule would return the same number and a
    // coarser one would be wrong, which is the sort of thing worth
    // knowing before trusting a volume to four decimal places.
    static const double points[5][3] = {{0.25, 0.25, 0.25},
                                        {0.5, 1.0 / 6.0, 1.0 / 6.0},
                                        {1.0 / 6.0, 0.5, 1.0 / 6.0},
                                        {1.0 / 6.0, 1.0 / 6.0, 0.5},
                                        {1.0 / 6.0, 1.0 / 6.0, 1.0 / 6.0}};
    static const double weights[5] = {-0.8 / 6.0, 0.45 / 6.0, 0.45 / 6.0, 0.45 / 6.0, 0.45 / 6.0};
    double total = 0.0;
    for (const std::array<int, 10> &element : mesh.tets10) {
        for (int q = 0; q < 5; ++q) {
            total += weights[q] *
                     JacobianAt(mesh.nodes, element, points[q][0], points[q][1], points[q][2]);
        }
    }
    return total;
}

}  // namespace fem
