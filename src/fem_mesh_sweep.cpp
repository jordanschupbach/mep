// Structured hexahedral meshing by sweeping (plans/CAD_FEM_PLAN.md G.6).
//
// WHY BOTHER, GIVEN A TETRAHEDRAL MESHER THAT WORKS. Tet4 is stiff in
// bending -- it locks, so a beam meshed with a few of them through its
// thickness comes out far too rigid -- and it is worse again under
// plasticity and near-incompressibility, where the constant strain over
// each element leaves no room for the volume-preserving part of the
// deformation to go. Hex8 has none of those problems, and a prismatic
// part is exactly the sort that gets bent. So the bodies that most want
// hexes are the ones whose shape makes hexes easy, which is why this is
// worth having even though it handles only some bodies.
//
// THE SHAPE OF THE ALGORITHM. A prismatic body is a profile times a
// length. Mesh the profile with quadrilaterals, copy that mesh up the
// length in layers, and join layer to layer: every quadrilateral becomes
// a column of hexahedra. The structure is exact -- the element count is
// quads times layers, the boundary is the profile mesh at top and bottom
// and the profile's boundary edges up the sides -- and none of it has to
// be discovered, which is the whole difference between this and the
// tetrahedral mesher.
//
// QUADRILATERALS FROM TRIANGLES, BY SUBDIVISION RATHER THAN BY PAIRING.
// The obvious way to get quads is to mesh with triangles and glue
// adjacent pairs together. It is obvious and it does not work well: the
// pairing is a matching problem, some triangles always end up with no
// partner, and a mesh that is mostly hexes with a few wedges scattered
// through it needs two element libraries to solve and two sets of
// quality rules to judge. Splitting each triangle into three quads
// instead -- corner, edge midpoint, centroid, other edge midpoint --
// always works, for any triangulation of any profile with any number of
// holes, and gives every element the same type. It costs three times as
// many elements, and each of them is better shaped than the triangle it
// came from was: an equilateral triangle yields three quads whose worst
// angle is sixty degrees.

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

// The six faces of a hexahedron, each wound so that it faces outwards.
// Used for the boundary and for nothing else, so it lives next to the
// ordering comment in the header rather than being rediscovered.
constexpr int kHexFaces[6][4] = {
    {0, 3, 2, 1},  // the 0-1-2-3 face, reversed so it faces away from 4-7
    {4, 5, 6, 7},  // the opposite one
    {0, 1, 5, 4}, {1, 2, 6, 5}, {2, 3, 7, 6}, {3, 0, 4, 7}};

std::array<int, 2> Key(int a, int b) {
    return a < b ? std::array<int, 2>{a, b} : std::array<int, 2>{b, a};
}

}  // namespace

double HexVolume(const std::vector<Vec3d> &nodes, const Hexahedron &hex) {
    // By the divergence theorem over its six faces, each split into two
    // triangles. Not by decomposing into tetrahedra, which needs a
    // choice of diagonals and gives a different answer for each.
    double total = 0.0;
    for (const auto &face : kHexFaces) {
        const Vec3d &p0 = nodes[static_cast<std::size_t>(hex.n[static_cast<std::size_t>(face[0])])];
        const Vec3d &p1 = nodes[static_cast<std::size_t>(hex.n[static_cast<std::size_t>(face[1])])];
        const Vec3d &p2 = nodes[static_cast<std::size_t>(hex.n[static_cast<std::size_t>(face[2])])];
        const Vec3d &p3 = nodes[static_cast<std::size_t>(hex.n[static_cast<std::size_t>(face[3])])];
        total += p0.Cross(p1).Dot(p2) + p0.Cross(p2).Dot(p3);
    }
    return total / 6.0;
}

double HexScaledJacobian(const std::vector<Vec3d> &nodes, const Hexahedron &hex) {
    // The three edges leaving each corner, in an order that is
    // right-handed for a cube wound as the header describes.
    static const int corner_edges[8][3] = {{1, 3, 4}, {2, 0, 5}, {3, 1, 6}, {0, 2, 7},
                                           {7, 5, 0}, {4, 6, 1}, {5, 7, 2}, {6, 4, 3}};
    double worst = 2.0;
    for (int i = 0; i < 8; ++i) {
        const Vec3d &at = nodes[static_cast<std::size_t>(hex.n[static_cast<std::size_t>(i)])];
        Vec3d edge[3];
        double length[3];
        bool degenerate = false;
        for (int k = 0; k < 3; ++k) {
            edge[k] = nodes[static_cast<std::size_t>(
                          hex.n[static_cast<std::size_t>(corner_edges[i][k])])] -
                      at;
            length[k] = edge[k].Length();
            if (!(length[k] > 0.0)) degenerate = true;
        }
        if (degenerate) return 0.0;
        const double scaled =
            edge[0].Cross(edge[1]).Dot(edge[2]) / (length[0] * length[1] * length[2]);
        worst = std::min(worst, scaled);
    }
    return worst > 1.0 ? 0.0 : worst;
}

bool CheckHexes(const VolumeMesh &mesh, std::string *error) {
    for (std::size_t i = 0; i < mesh.hexes.size(); ++i) {
        const Hexahedron &hex = mesh.hexes[i];
        for (const int node : hex.n) {
            if (node >= 0 && node < mesh.NodeCount()) continue;
            *error = "hexahedron " + std::to_string(i) + " refers to node " +
                     std::to_string(node) + ", which does not exist";
            return false;
        }
        std::array<int, 8> sorted = hex.n;
        std::sort(sorted.begin(), sorted.end());
        if (std::adjacent_find(sorted.begin(), sorted.end()) != sorted.end()) {
            *error = "hexahedron " + std::to_string(i) + " uses the same node twice";
            return false;
        }
        if (HexScaledJacobian(mesh.nodes, hex) > 0.0) continue;
        *error = "hexahedron " + std::to_string(i) +
                 " is flat or turned inside out at one of its corners";
        return false;
    }
    std::map<std::array<int, 4>, int> uses;
    for (const Hexahedron &hex : mesh.hexes) {
        for (const auto &face : kHexFaces) {
            std::array<int, 4> key{};
            for (int k = 0; k < 4; ++k) {
                key[static_cast<std::size_t>(k)] = hex.n[static_cast<std::size_t>(face[k])];
            }
            std::sort(key.begin(), key.end());
            ++uses[key];
        }
    }
    for (const auto &entry : uses) {
        if (entry.second <= 2) continue;
        *error = "a face is shared by " + std::to_string(entry.second) +
                 " hexahedra, which is one more than a mesh allows";
        return false;
    }
    // AND THE BOUNDARY CLOSES. Counted on *directed* edges of the faces
    // that belong to one element, each of which must appear once forward
    // and once backward -- which checks that the boundary is a closed
    // surface and that its quadrilaterals agree on which way is out, in
    // one pass. Counting undirected edges would pass a boundary with two
    // faces wound opposite ways, which is a mesh with an inside-out patch
    // in it.
    std::map<std::array<int, 2>, int> directed;
    for (const Hexahedron &hex : mesh.hexes) {
        for (const auto &face : kHexFaces) {
            std::array<int, 4> corner{};
            for (int k = 0; k < 4; ++k) {
                corner[static_cast<std::size_t>(k)] = hex.n[static_cast<std::size_t>(face[k])];
            }
            std::array<int, 4> key = corner;
            std::sort(key.begin(), key.end());
            if (uses[key] != 1) continue;
            for (int k = 0; k < 4; ++k) {
                const int u = corner[static_cast<std::size_t>(k)];
                const int v = corner[static_cast<std::size_t>((k + 1) % 4)];
                ++directed[std::array<int, 2>{u, v}];
                --directed[std::array<int, 2>{v, u}];
            }
        }
    }
    for (const auto &entry : directed) {
        if (entry.second == 0) continue;
        *error = "the boundary of the hexahedral mesh does not close: the edge between nodes " +
                 std::to_string(entry.first[0]) + " and " + std::to_string(entry.first[1]) +
                 " is not matched by one running the other way";
        return false;
    }
    return true;
}

bool MeshSweep(const cad::Model &model, cad::EntityId body_id, const SweepMeshOptions &options,
               VolumeMesh *out, MeshReport *report) {
    *out = VolumeMesh{};
    *report = MeshReport{};
    const cad::Body *body = model.GetBody(body_id);
    if (body == nullptr || body->shells.empty()) {
        report->error = "there is no such body to sweep";
        return false;
    }
    std::vector<cad::EntityId> faces;
    for (const cad::EntityId shell_id : body->shells) {
        const cad::Shell *shell = model.GetShell(shell_id);
        if (shell == nullptr) continue;
        for (const cad::EntityId face : shell->faces) faces.push_back(face);
    }
    if (faces.size() < 3) {
        report->error = "a body with fewer than three faces is not a swept solid";
        return false;
    }

    // --- Is it prismatic, and which way does it go? ------------------------
    //
    // A pair of planar faces whose outward normals are exactly opposite,
    // with every other face parallel to the line between them. That is
    // the definition, and it is checked rather than assumed: the whole
    // value of a structured mesher is that its structure is guaranteed,
    // and a body that is nearly prismatic would give a mesh that is
    // nearly right, which is no use at all.
    const double angular = options.surface.tolerance.angular;
    auto plane_of = [&](cad::EntityId face_id, Vec3d *normal, Vec3d *point) {
        const cad::Face *face = model.GetFace(face_id);
        if (face == nullptr) return false;
        const cad::Surface *surface = model.SurfaceAt(face->surface);
        if (surface == nullptr || surface->Kind() != cad::SurfaceKind::Plane) return false;
        double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
        surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
        const double u = (u_lo + u_hi) * 0.5;
        const double v = (v_lo + v_hi) * 0.5;
        *point = surface->Point(u, v);
        *normal = model.FaceNormal(face_id, u, v);
        return normal->LengthSquared() > 0.0;
    };

    // EVERY CANDIDATE PAIR IS TRIED, NOT THE FIRST ONE THAT LOOKS RIGHT.
    // A drilled block has three pairs of parallel faces and only one of
    // them is the sweep: take the block's sides and the hole is not
    // parallel to them, so the body reads as unsweepable when it sweeps
    // perfectly well the other way up. Committing to the first pair
    // found turned a good body into an error message.
    struct Candidate {
        cad::EntityId source = cad::kNoEntity;
        cad::EntityId target = cad::kNoEntity;
        Vec3d direction{};
        double distance = 0.0;
    };
    std::vector<Candidate> candidates;
    for (std::size_t i = 0; i < faces.size(); ++i) {
        Vec3d normal_i, point_i;
        if (!plane_of(faces[i], &normal_i, &point_i)) continue;
        for (std::size_t j = 0; j < faces.size(); ++j) {
            if (i == j) continue;
            Vec3d normal_j, point_j;
            if (!plane_of(faces[j], &normal_j, &point_j)) continue;
            const Vec3d a = normal_i.Normalized();
            const Vec3d b = normal_j.Normalized();
            if (a.Dot(b) > -std::cos(angular)) continue;
            // The sweep goes from face i towards face j, which is the
            // way face i's *inward* normal points.
            const Vec3d along = a * -1.0;
            const double gap = (point_j - point_i).Dot(along);
            if (!(gap > options.surface.tolerance.linear)) continue;
            candidates.push_back(Candidate{faces[i], faces[j], along, gap});
        }
    }
    if (candidates.empty()) {
        report->error = "this body has no pair of parallel planar faces facing away from each "
                        "other, so there is nothing to sweep between";
        return false;
    }

    // A side face must contain the sweep direction: its normal is
    // perpendicular to it everywhere it is sampled. A cylinder whose axis
    // is along the sweep passes this as readily as a plane, which is
    // right -- a rounded profile, or a hole through the profile, sweeps
    // perfectly well.
    auto sides_are_parallel = [&](const Candidate &candidate) {
        for (const cad::EntityId face_id : faces) {
            if (face_id == candidate.source || face_id == candidate.target) continue;
            const cad::Face *face = model.GetFace(face_id);
            const cad::Surface *surface = face != nullptr ? model.SurfaceAt(face->surface) : nullptr;
            if (surface == nullptr) continue;
            double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
            surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
            for (int i = 0; i <= 3; ++i) {
                for (int j = 0; j <= 3; ++j) {
                    const double u = u_lo + (u_hi - u_lo) * i / 3.0;
                    const double v = v_lo + (v_hi - v_lo) * j / 3.0;
                    const Vec3d normal = model.FaceNormal(face_id, u, v);
                    if (!(normal.LengthSquared() > 0.0)) continue;
                    if (std::fabs(normal.Normalized().Dot(candidate.direction)) <
                        std::sin(angular)) {
                        continue;
                    }
                    return false;
                }
            }
        }
        return true;
    };

    cad::EntityId source = cad::kNoEntity;
    cad::EntityId target = cad::kNoEntity;
    Vec3d direction{};
    double distance = 0.0;
    for (const Candidate &candidate : candidates) {
        if (!sides_are_parallel(candidate)) continue;
        source = candidate.source;
        target = candidate.target;
        direction = candidate.direction;
        distance = candidate.distance;
        break;
    }
    if (source == cad::kNoEntity) {
        report->error = "a side face of this body is not parallel to the sweep direction, so the "
                        "body is not a profile swept along a line and cannot be meshed with "
                        "hexahedra this way";
        return false;
    }

    // --- The profile, meshed with triangles and then quadrangulated --------
    SizingField sizing;
    SizingOptions sizing_options = options.sizing;
    if (sizing_options.target == 0.0) sizing_options = options.surface.sizing;
    if (!sizing.Build(model, {body_id}, sizing_options, &report->error)) return false;
    SurfaceMesh surface;
    MeshReport surface_report;
    if (!MeshSurface(model, {body_id}, options.surface, sizing, &surface, &surface_report)) {
        report->error = surface_report.error;
        return false;
    }
    for (const std::string &warning : surface_report.warnings) {
        report->warnings.push_back(warning);
    }

    // Only the source face's triangles, and only the nodes they use.
    std::map<int, int> renumbered;
    std::vector<Vec3d> profile;
    std::vector<SurfaceMesh::Provenance> profile_from;
    auto take = [&](int node) {
        const auto found = renumbered.find(node);
        if (found != renumbered.end()) return found->second;
        const int index = static_cast<int>(profile.size());
        renumbered[node] = index;
        profile.push_back(surface.nodes[static_cast<std::size_t>(node)]);
        profile_from.push_back(surface.provenance[static_cast<std::size_t>(node)]);
        return index;
    };
    std::vector<std::array<int, 3>> triangles;
    for (const SurfaceTriangle &t : surface.triangles) {
        if (t.face != source) continue;
        triangles.push_back({take(t.a), take(t.b), take(t.c)});
    }
    if (triangles.empty()) {
        report->error = "the source face produced no triangles to sweep";
        return false;
    }

    // The source face's outward normal is away from the body, so its
    // triangles are wound against the sweep direction; turning them round
    // here is what makes every hexahedron come out the right way up
    // without a per-element test.
    //
    // EACH TRIANGLE ON ITS OWN, not the whole set according to the first
    // one. The first version read the winding of triangle zero and
    // flipped all of them together, which is correct exactly as long as
    // the surface mesher hands back a consistently wound face -- and it
    // does for a rectangle and does not for a skewed or tapered one. The
    // symptom was "hexahedron 3 is flat or turned inside out" on a
    // rhombic plate and a tapered strip, both of which are as prismatic
    // as a body gets, and both of which sweep perfectly well once each
    // triangle is asked which way round it is. The test costs one cross
    // product per triangle and removes a dependence on somebody else's
    // invariant.
    for (std::array<int, 3> &t : triangles) {
        const Vec3d &a = profile[static_cast<std::size_t>(t[0])];
        const Vec3d &b = profile[static_cast<std::size_t>(t[1])];
        const Vec3d &c = profile[static_cast<std::size_t>(t[2])];
        if ((b - a).Cross(c - a).Dot(direction) < 0.0) std::swap(t[1], t[2]);
    }

    // WHICH CAD EDGES A PROFILE NODE LIES ON. A node the surface mesher
    // put on an edge lies on that one; a node it put on a vertex lies on
    // every edge meeting there. Both are needed, because a profile edge
    // running between two corners of the profile has a node of each kind
    // at its ends and neither alone identifies it.
    std::map<cad::EntityId, std::vector<cad::EntityId>> edges_of_vertex;
    for (const cad::Edge &edge : model.Edges()) {
        edges_of_vertex[edge.start_vertex].push_back(edge.id);
        if (edge.end_vertex != edge.start_vertex) {
            edges_of_vertex[edge.end_vertex].push_back(edge.id);
        }
    }
    std::vector<std::vector<cad::EntityId>> on_edges(profile.size());
    auto record_edges = [&](int node) {
        const SurfaceMesh::Provenance &from = profile_from[static_cast<std::size_t>(node)];
        std::vector<cad::EntityId> &into = on_edges[static_cast<std::size_t>(node)];
        into.clear();
        if (from.edge != cad::kNoEntity) {
            into.push_back(from.edge);
            return;
        }
        if (from.vertex == cad::kNoEntity) return;
        const auto found = edges_of_vertex.find(from.vertex);
        if (found != edges_of_vertex.end()) into = found->second;
    };
    for (std::size_t i = 0; i < profile.size(); ++i) record_edges(static_cast<int>(i));
    auto shared_edge = [&](int u, int v) {
        for (const cad::EntityId a : on_edges[static_cast<std::size_t>(u)]) {
            for (const cad::EntityId b : on_edges[static_cast<std::size_t>(v)]) {
                if (a == b) return a;
            }
        }
        return cad::kNoEntity;
    };
    auto side_face_of = [&](cad::EntityId edge) {
        if (edge == cad::kNoEntity) return cad::kNoEntity;
        for (const cad::EntityId face : model.FacesOfEdge(edge)) {
            if (face != source && face != target) return face;
        }
        return cad::kNoEntity;
    };

    // Each triangle into three quadrilaterals: corner, the midpoint of
    // one of its edges, the centroid, the midpoint of the other.
    std::map<std::array<int, 2>, int> edge_middle;
    std::vector<std::array<int, 4>> quads;
    for (const std::array<int, 3> &t : triangles) {
        int middle[3];
        for (int k = 0; k < 3; ++k) {
            const int u = t[static_cast<std::size_t>(k)];
            const int v = t[static_cast<std::size_t>((k + 1) % 3)];
            const std::array<int, 2> key = Key(u, v);
            const auto found = edge_middle.find(key);
            if (found != edge_middle.end()) {
                middle[k] = found->second;
                continue;
            }
            middle[k] = static_cast<int>(profile.size());
            profile.push_back((profile[static_cast<std::size_t>(u)] +
                               profile[static_cast<std::size_t>(v)]) *
                              0.5);
            // A midpoint of two nodes of the same CAD edge is on that
            // edge; anything else is inside the face. Kept so that the
            // swept side faces can still be named.
            SurfaceMesh::Provenance where;
            where.edge = shared_edge(u, v);
            profile_from.push_back(where);
            on_edges.push_back(where.edge == cad::kNoEntity
                                   ? std::vector<cad::EntityId>{}
                                   : std::vector<cad::EntityId>{where.edge});
            edge_middle[key] = middle[k];
        }
        const int centre = static_cast<int>(profile.size());
        profile.push_back((profile[static_cast<std::size_t>(t[0])] +
                           profile[static_cast<std::size_t>(t[1])] +
                           profile[static_cast<std::size_t>(t[2])]) *
                          (1.0 / 3.0));
        profile_from.push_back({});
        on_edges.push_back({});
        for (int k = 0; k < 3; ++k) {
            quads.push_back({t[static_cast<std::size_t>(k)], middle[k], centre,
                             middle[(k + 2) % 3]});
        }
    }

    // The profile's own boundary: the quadrilateral edges used once.
    // These are what sweep into the side faces, and knowing them is what
    // makes the boundary something built rather than something found.
    //
    // KEPT IN THE DIRECTION ITS OWN QUADRILATERAL RUNS IT, which is the
    // whole of a bug that took a benchmark to find. They used to be
    // stored as the *sorted* pair, because that is the key the
    // used-once count is kept under -- and a sorted pair has lost the
    // one thing the side quadrilateral needs, which is which way round
    // to be wound. The side faces then came out with their normals
    // pointing in or out according to how the nodes happened to be
    // numbered, and nothing noticed: CheckHexes verifies that the
    // boundary closes, but it does that from the hexahedra, not from
    // these quadrilaterals.
    //
    // What did notice was NAFEMS LE7. A pressure is applied along the
    // *inward* normal of the facet it acts on, so a facet wound the
    // wrong way pressurises the cylinder from the outside: the bore
    // moved inward under internal pressure, the hoop stress came out at
    // a third of Lame's and with the wrong sign at the outside, and the
    // model was otherwise perfectly well behaved. The profile
    // quadrilaterals are wound so their normal is along the sweep, so
    // the rim taken in their order runs anticlockwise seen from the far
    // end, and {below u, below v, above v, above u} then faces out.
    std::vector<std::array<int, 2>> profile_rim;

    // --- Smooth the profile, once, before it is copied everywhere ----------
    //
    // Every layer inherits the profile, so a pass here is worth as many
    // passes as there are layers afterwards. Boundary nodes are pinned:
    // the profile's boundary is the body's, and moving it would move the
    // side faces off the model.
    {
        std::map<std::array<int, 2>, int> uses;
        std::map<std::array<int, 2>, std::array<int, 2>> as_wound;
        for (const std::array<int, 4> &q : quads) {
            for (int k = 0; k < 4; ++k) {
                const int u = q[static_cast<std::size_t>(k)];
                const int v = q[static_cast<std::size_t>((k + 1) % 4)];
                ++uses[Key(u, v)];
                as_wound[Key(u, v)] = std::array<int, 2>{u, v};
            }
        }
        std::vector<bool> pinned(profile.size(), false);
        for (const auto &entry : uses) {
            if (entry.second == 1) profile_rim.push_back(as_wound[entry.first]);
        }
        std::vector<std::vector<int>> neighbours(profile.size());
        for (const auto &entry : uses) {
            const int u = entry.first[0];
            const int v = entry.first[1];
            if (entry.second == 1) {
                pinned[static_cast<std::size_t>(u)] = true;
                pinned[static_cast<std::size_t>(v)] = true;
            }
            neighbours[static_cast<std::size_t>(u)].push_back(v);
            neighbours[static_cast<std::size_t>(v)].push_back(u);
        }
        for (int pass = 0; pass < std::max(0, options.smoothing_passes); ++pass) {
            for (std::size_t i = 0; i < profile.size(); ++i) {
                if (pinned[i] || neighbours[i].empty()) continue;
                Vec3d sum{};
                for (const int other : neighbours[i]) {
                    sum = sum + profile[static_cast<std::size_t>(other)];
                }
                const Vec3d target_at = sum * (1.0 / static_cast<double>(neighbours[i].size()));
                const Vec3d was = profile[i];
                profile[i] = was + (target_at - was) * 0.5;
                // Only if every quadrilateral it belongs to stays convex
                // and the right way round, tested by the same corner
                // cross products the hexahedra will be judged by.
                bool ok = true;
                for (const std::array<int, 4> &q : quads) {
                    if (q[0] != static_cast<int>(i) && q[1] != static_cast<int>(i) &&
                        q[2] != static_cast<int>(i) && q[3] != static_cast<int>(i)) {
                        continue;
                    }
                    for (int k = 0; k < 4 && ok; ++k) {
                        const Vec3d &at = profile[static_cast<std::size_t>(q[static_cast<std::size_t>(k)])];
                        const Vec3d e0 =
                            profile[static_cast<std::size_t>(q[static_cast<std::size_t>((k + 1) % 4)])] - at;
                        const Vec3d e1 =
                            profile[static_cast<std::size_t>(q[static_cast<std::size_t>((k + 3) % 4)])] - at;
                        if (e0.Cross(e1).Dot(direction) >= 0.0) ok = false;
                    }
                    if (!ok) break;
                }
                if (!ok) profile[i] = was;
            }
        }
    }

    // --- Sweep -------------------------------------------------------------
    int layers = options.layers;
    if (layers <= 0) {
        double wanted = 0.0;
        for (const Vec3d &p : profile) wanted += sizing.At(p);
        wanted /= static_cast<double>(profile.size());
        layers = std::max(1, static_cast<int>(std::lround(distance / std::max(wanted, 1e-12))));
    }
    const int per_layer = static_cast<int>(profile.size());
    out->nodes.reserve(static_cast<std::size_t>(per_layer) * static_cast<std::size_t>(layers + 1));
    for (int layer = 0; layer <= layers; ++layer) {
        const double along = distance * layer / layers;
        for (std::size_t i = 0; i < profile.size(); ++i) {
            out->nodes.push_back(profile[i] + direction * along);
            SurfaceMesh::Provenance where = profile_from[i];
            if (layer == 0) {
                where.face = source;
            } else if (layer == layers) {
                where.face = target;
            } else if (where.edge != cad::kNoEntity) {
                // A node that was on a CAD edge of the profile sweeps up
                // a side face; the faces meeting that edge are the ones
                // it is now on.
                const std::vector<cad::EntityId> sides = model.FacesOfEdge(where.edge);
                for (const cad::EntityId side : sides) {
                    if (side != source && side != target) {
                        where.face = side;
                        break;
                    }
                }
                where.edge = cad::kNoEntity;
            } else {
                where = {};
            }
            out->provenance.push_back(where);
        }
    }
    for (int layer = 0; layer < layers; ++layer) {
        const int below = layer * per_layer;
        const int above = (layer + 1) * per_layer;
        for (const std::array<int, 4> &q : quads) {
            Hexahedron hex;
            for (int k = 0; k < 4; ++k) {
                hex.n[static_cast<std::size_t>(k)] = below + q[static_cast<std::size_t>(k)];
                hex.n[static_cast<std::size_t>(k + 4)] = above + q[static_cast<std::size_t>(k)];
            }
            out->hexes.push_back(hex);
        }
    }

    // --- The boundary, built rather than found -----------------------------
    //
    // A swept mesh's boundary is known before it is made: the profile at
    // the bottom, the profile at the top, and the profile's own rim
    // carried up each layer. The tetrahedral mesher has to find its
    // boundary by looking for faces used once, because it does not know
    // in advance which elements it will keep; here that would be work
    // done twice and, worse, would leave each quadrilateral to guess
    // which CAD face it came from. Built this way every one of them
    // knows, which is what Part H.2 needs to put a pressure on a face.
    const int top = layers * per_layer;
    for (const std::array<int, 4> &q : quads) {
        // The bottom faces away from the sweep and the top towards it,
        // so the bottom is the profile reversed.
        out->boundary_quads.push_back(SurfaceQuad{q[3], q[2], q[1], q[0], source});
        out->boundary_quads.push_back(
            SurfaceQuad{top + q[0], top + q[1], top + q[2], top + q[3], target});
    }
    int unnamed_sides = 0;
    for (const std::array<int, 2> &rim : profile_rim) {
        const cad::EntityId side = side_face_of(shared_edge(rim[0], rim[1]));
        if (side == cad::kNoEntity) ++unnamed_sides;
        for (int layer = 0; layer < layers; ++layer) {
            const int below = layer * per_layer;
            const int above = (layer + 1) * per_layer;
            out->boundary_quads.push_back(SurfaceQuad{below + rim[0], below + rim[1],
                                                      above + rim[1], above + rim[0], side});
        }
    }
    if (unnamed_sides > 0) {
        report->warnings.push_back(
            std::to_string(unnamed_sides) +
            " edges of the profile could not be traced back to a side face, so the quadrilaterals "
            "swept from them are not named and nothing can be attached to them by face");
    }

    report->nodes = out->NodeCount();
    report->tetrahedra = 0;
    report->triangles = static_cast<int>(out->boundary_quads.size());
    const MeshQuality quality = MeasureQuality(*out);
    report->min_scaled_jacobian = quality.min_scaled_jacobian;
    report->worst_aspect = quality.worst_aspect;
    report->min_dihedral = quality.min_dihedral;
    report->max_dihedral = quality.max_dihedral;
    std::string error;
    if (!CheckMesh(*out, &error)) {
        report->error = error;
        return false;
    }
    report->ok = true;
    return true;
}

}  // namespace fem
