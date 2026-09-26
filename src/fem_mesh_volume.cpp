// Tetrahedralisation and quality (plans/CAD_FEM_PLAN.md Parts G.3, G.4
// and G.7).
//
// Delaunay by Bowyer-Watson in three dimensions, with a vertex at
// infinity rather than a bounding tetrahedron -- the same formulation
// cad_tessellate.cpp uses in two, and for the same reason recorded
// there: what has to be contained is not the points but the
// circumspheres, and those are unbounded relative to the point set, so
// no bounding tetrahedron is big enough and one that is too small leaves
// holes in the middle of the result.
//
// THE HARD PART IS NOT THE DELAUNAY, IT IS THE BOUNDARY. A Delaunay
// tetrahedralisation of the surface mesh's nodes need not contain the
// surface mesh's triangles: in three dimensions, unlike two, a
// triangulation of a point set can fail to conform to a given surface
// however the points are arranged (Schonhardt's polyhedron is the
// standard example, and it cannot be tetrahedralised at all without
// extra points).
//
// The textbook answer is Steiner points -- split each missing triangle
// and insert the pieces until it comes back. That was written, and it
// diverged: five thousand of them left nine thousand triangles still
// missing on a cube. Almost every missing triangle was missing for a
// reason no amount of splitting addresses. The nodes of a planar face
// are exactly coplanar, so which diagonal each quadrilateral among them
// gets is a tie; the surface mesher broke it one way and the
// tetrahedraliser the other; and splitting a coplanar triangle adds
// another coplanar node and another tie.
//
// So the surface mesher's triangles are not the target here. Its nodes
// are. A different triangulation of the same nodes spans the same
// polygon on a planar face and interpolates the same points on a curved
// one, so the boundary is read off the elements that were kept -- every
// face used by exactly one of them -- which is watertight because a face
// of a tetrahedralisation belongs to one element or two and there is no
// third possibility. Which elements are kept is decided one at a time,
// against the surface; the comment at that code says why the cheaper
// thing there does not work either.
//
// What remains genuinely unhandled is Schonhardt: a body no
// tetrahedralisation of its own boundary nodes can fill. It is reported
// by the volume not adding up rather than silently meshed wrong.

#include "cad_predicates.h"
#include "fem_mesh.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cmath>
#include <cstdint>
#include <limits>
#include <map>
#include <set>
#include <utility>

namespace fem {
namespace {

using cad::Vec3d;

struct Tet {
    int a = 0, b = 0, c = 0, d = 0;
    bool alive = true;
};

std::array<double, 3> Coord(const std::vector<Vec3d> &points, int i) {
    const Vec3d &p = points[static_cast<std::size_t>(i)];
    return {p.x, p.y, p.z};
}

// ORIENTATION, ONCE, EXPLICITLY. Orient3D's sign convention is
// Shewchuk's -- positive when d lies *below* the plane abc -- which is
// the opposite of the right-hand rule the volume formula uses, so a
// tetrahedron with positive Orient3D has negative volume by that formula
// and the other way about. Deriving each face's winding from the other
// convention is how this went wrong the first time: 200 random points
// produced 328457 tetrahedra with fifty thousand faces belonging to more
// than two of them, which is not a mesh of anything. So nothing here
// derives an orientation. Every tetrahedron is ordered by asking
// Orient3D, and every face is ordered by asking it again.
double TetVolume(const Vec3d &a, const Vec3d &b, const Vec3d &c, const Vec3d &d) {
    return (b - a).Cross(c - a).Dot(d - a) / 6.0;
}

// The face of `t` opposite vertex `which`, ordered so that the omitted
// vertex lies below it -- so the tetrahedron the face came from is on
// the positive side, and so is any point in the cavity dug out of it.
std::array<int, 3> FaceOpposite(const std::vector<Vec3d> &points, const Tet &t, int which) {
    const int v[4] = {t.a, t.b, t.c, t.d};
    std::array<int, 3> face{};
    int at = 0;
    for (int i = 0; i < 4; ++i) {
        if (i != which) face[static_cast<std::size_t>(at++)] = v[i];
    }
    if (cad::Orient3D(Coord(points, face[0]).data(), Coord(points, face[1]).data(),
                      Coord(points, face[2]).data(), Coord(points, v[which]).data()) < 0) {
        std::swap(face[1], face[2]);
    }
    return face;
}


// Delaunay of a point set. `infinity` is the index one past the last
// point and stands for the vertex at infinity.
//
// TIES ARE BROKEN SYMBOLICALLY, NOT BY MOVING THE POINTS. Bowyer-Watson
// only works when the cavity a new point digs is star-shaped, and that
// is only guaranteed when no two competing answers are exactly tied. On
// the inputs a mesher actually sees the ties are everywhere: a planar
// face's nodes are exactly coplanar -- that is what planar *means* --
// and a regular lattice is cospherical eight points at a time. The
// symptom is unmistakable and was how this was found: faces shared by
// six tetrahedra, which is not a mesh of anything.
//
// Nudging the coordinates fixes the ties and breaks the mesh: boundary
// nodes stop being coplanar, so the surface triangles stop being faces
// of the result, and boundary recovery then chases a target that moves.
// So the points are left exactly where they are and the tie is broken in
// the one place it is a tie -- by lifting point i onto the paraboloid a
// hair higher than its neighbours, by e^i for an infinitesimal e. That
// is a regular (weighted) triangulation with weights too small to hide
// any point, and it has no ties at all.
//
// InSphere is the determinant of the lifted points, so raising one
// point's lift moves that determinant by the cofactor of its entry --
// which is, up to sign, an Orient3D of the other four. So when InSphere
// is exactly zero the answer is the first non-zero such Orient3D, taken
// in order of the dominant point, which is the one with the smallest
// index. The signs below are not derived: they were measured against a
// direct evaluation of the 5x5 determinant, and they alternate because
// the cofactor does.
void Tetrahedralise(const std::vector<Vec3d> &original, std::vector<Tet> *out) {
    out->clear();
    const int count = static_cast<int>(original.size());
    const std::vector<Vec3d> &points = original;
    if (count < 4) return;
    const int infinity = count;

    // A non-degenerate starting tetrahedron.
    int second = -1;
    for (int i = 1; i < count && second < 0; ++i) {
        if ((points[static_cast<std::size_t>(i)] - points[0]).LengthSquared() > 0.0) second = i;
    }
    if (second < 0) return;
    int third = -1;
    for (int i = second + 1; i < count && third < 0; ++i) {
        const Vec3d cross = (points[static_cast<std::size_t>(second)] - points[0])
                                .Cross(points[static_cast<std::size_t>(i)] - points[0]);
        if (cross.LengthSquared() > 0.0) third = i;
    }
    if (third < 0) return;
    int fourth = -1;
    for (int i = third + 1; i < count && fourth < 0; ++i) {
        if (cad::Orient3D(Coord(points, 0).data(), Coord(points, second).data(),
                          Coord(points, third).data(), Coord(points, i).data()) != 0) {
            fourth = i;
        }
    }
    if (fourth < 0) return;

    std::vector<Tet> tets;
    Tet seed{0, second, third, fourth, true};
    if (cad::Orient3D(Coord(points, seed.a).data(), Coord(points, seed.b).data(),
                      Coord(points, seed.c).data(), Coord(points, seed.d).data()) < 0) {
        std::swap(seed.c, seed.d);
    }
    tets.push_back(seed);
    // A ghost beyond each of the seed's four faces. A ghost tetrahedron
    // (x, y, z, INF) stands for the half space beyond hull face (x, y, z),
    // stored so that the *inside* is on the positive side -- so a point
    // outside is on the negative side, which is one Orient3D test.
    for (int which = 0; which < 4; ++which) {
        const std::array<int, 3> face = FaceOpposite(points, seed, which);
        tets.push_back(Tet{face[0], face[1], face[2], infinity, true});
    }
    const Vec3d inside = (points[static_cast<std::size_t>(seed.a)] +
                          points[static_cast<std::size_t>(seed.b)] +
                          points[static_cast<std::size_t>(seed.c)] +
                          points[static_cast<std::size_t>(seed.d)]) * 0.25;
    const std::array<double, 3> inside_coord{inside.x, inside.y, inside.z};

    // InSphere with the tie broken as described above.
    auto in_sphere = [&](int a, int b, int c, int d, int p) {
        const int v[5] = {a, b, c, d, p};
        std::array<std::array<double, 3>, 5> at{};
        for (int k = 0; k < 5; ++k) at[static_cast<std::size_t>(k)] = Coord(points, v[k]);
        const int s = cad::InSphere(at[0].data(), at[1].data(), at[2].data(), at[3].data(),
                                    at[4].data());
        if (s != 0) return s > 0;
        int order[5] = {0, 1, 2, 3, 4};
        for (int k = 1; k < 5; ++k) {
            for (int m = k; m > 0 && v[order[m]] < v[order[m - 1]]; --m) {
                std::swap(order[m], order[m - 1]);
            }
        }
        for (const int j : order) {
            const double *other[4];
            int filled = 0;
            for (int r = 0; r < 5; ++r) {
                if (r != j) other[filled++] = at[static_cast<std::size_t>(r)].data();
            }
            const int side = cad::Orient3D(other[0], other[1], other[2], other[3]);
            if (side == 0) continue;
            return (j % 2 == 1) ? side > 0 : side < 0;
        }
        // All five points lie in a plane: the determinant is identically
        // zero and no perturbation of the lift alone can break it. The
        // ghost test below is what decides these.
        return false;
    };

    // A point exactly coplanar with a hull facet is the common case,
    // not the rare one -- every node of a box's face is coplanar with
    // that face -- and the rule for it is not "beyond the hull" either
    // way round. It is beyond the ghost exactly when it falls inside
    // that facet's circumcircle, which is the two-dimensional Delaunay
    // question asked within the plane.
    //
    // That question needs no separate predicate. A hull facet borders
    // exactly one real tetrahedron, and a sphere through the facet's
    // three points cuts their plane in their circumcircle whatever the
    // fourth point is -- so for a point already in that plane, "inside
    // the facet's circumcircle" and "inside the neighbour's
    // circumsphere" are the same question. So the coplanar ghosts are
    // decided from which real tetrahedra died, which also makes the two
    // tests consistent by construction rather than by argument, and
    // consistency is the whole of what the cavity needs to stay
    // star-shaped.
    std::vector<std::array<int, 3>> cavity;
    std::vector<std::array<int, 3>> opened;
    std::size_t dead = 0;
    for (int i = 0; i < count; ++i) {
        if (i == 0 || i == second || i == third || i == fourth) continue;
        if (dead * 2 > tets.size()) {
            tets.erase(std::remove_if(tets.begin(), tets.end(), [](const Tet &t) { return !t.alive; }),
                       tets.end());
            dead = 0;
        }
        cavity.clear();
        opened.clear();
        const std::array<double, 3> point = Coord(points, i);
        for (Tet &t : tets) {
            if (!t.alive || t.d == infinity) continue;
            if (!in_sphere(t.a, t.b, t.c, t.d, i)) continue;
            t.alive = false;
            ++dead;
            for (int which = 0; which < 4; ++which) {
                const std::array<int, 3> face = FaceOpposite(points, t, which);
                cavity.push_back(face);
                std::array<int, 3> key = face;
                std::sort(key.begin(), key.end());
                opened.push_back(key);
            }
        }
        std::sort(opened.begin(), opened.end());
        for (Tet &t : tets) {
            if (!t.alive || t.d != infinity) continue;
            const int side =
                cad::Orient3D(Coord(points, t.a).data(), Coord(points, t.b).data(),
                              Coord(points, t.c).data(), point.data());
            if (side > 0) continue;
            if (side == 0) {
                std::array<int, 3> key{t.a, t.b, t.c};
                std::sort(key.begin(), key.end());
                if (!std::binary_search(opened.begin(), opened.end(), key)) continue;
            }
            t.alive = false;
            ++dead;
            cavity.push_back({t.a, t.b, t.c});
            cavity.push_back({t.a, t.b, infinity});
            cavity.push_back({t.b, t.c, infinity});
            cavity.push_back({t.c, t.a, infinity});
        }
        if (cavity.empty()) continue;
        std::map<std::array<int, 3>, int> seen;
        for (std::array<int, 3> face : cavity) {
            std::sort(face.begin(), face.end());
            ++seen[face];
        }
        for (const std::array<int, 3> &face : cavity) {
            std::array<int, 3> key = face;
            std::sort(key.begin(), key.end());
            if (seen[key] != 1) continue;
            if (face[0] == infinity || face[1] == infinity || face[2] == infinity) {
                int x = -1;
                int y = -1;
                for (int v : face) {
                    if (v == infinity) continue;
                    if (x < 0) {
                        x = v;
                    } else {
                        y = v;
                    }
                }
                if (x < 0 || y < 0) continue;
                std::array<int, 3> hull{x, y, i};
                const int side =
                    cad::Orient3D(Coord(points, hull[0]).data(), Coord(points, hull[1]).data(),
                                  Coord(points, hull[2]).data(), inside_coord.data());
                // Three collinear points bound no half space, so a ghost
                // on them is a face with no inside and no outside --
                // which is one way a face ends up belonging to more than
                // two elements. A regular lattice has these in every row.
                if (side == 0) continue;
                if (side < 0) std::swap(hull[1], hull[2]);
                tets.push_back(Tet{hull[0], hull[1], hull[2], infinity, true});
            } else {
                Tet made{face[0], face[1], face[2], i, true};
                const int orientation =
                    cad::Orient3D(Coord(points, made.a).data(), Coord(points, made.b).data(),
                                  Coord(points, made.c).data(), Coord(points, made.d).data());
                if (orientation == 0) continue;
                if (orientation < 0) std::swap(made.b, made.c);
                tets.push_back(made);
            }
        }
    }

    for (const Tet &t : tets) {
        if (t.alive && t.d != infinity) out->push_back(t);
    }
}

// Is a point inside the closed surface mesh? Ray parity along a fixed
// direction, with the ray nudged off any vertex or edge it happens to
// graze by trying another direction.
// The nearest point of a triangle to a point, and how far it is.
double DistanceToTriangle(const Vec3d &point, const Vec3d &p0, const Vec3d &p1, const Vec3d &p2,
                          Vec3d *closest) {
    const Vec3d edge0 = p1 - p0;
    const Vec3d edge1 = p2 - p0;
    const Vec3d from = p0 - point;
    const double a = edge0.Dot(edge0);
    const double b = edge0.Dot(edge1);
    const double c = edge1.Dot(edge1);
    const double d = edge0.Dot(from);
    const double e = edge1.Dot(from);
    double determinant = a * c - b * b;
    if (determinant <= 0.0) determinant = 1e-300;
    double s = (b * e - c * d) / determinant;
    double t = (b * d - a * e) / determinant;
    // Clamped onto the triangle, corners and edges included.
    if (s < 0.0) s = 0.0;
    if (t < 0.0) t = 0.0;
    if (s + t > 1.0) {
        const double over = (s + t - 1.0) * 0.5;
        s -= over;
        t -= over;
        if (s < 0.0) {
            s = 0.0;
            t = std::min(1.0, std::max(0.0, -e / (c > 0.0 ? c : 1e-300)));
        } else if (t < 0.0) {
            t = 0.0;
            s = std::min(1.0, std::max(0.0, -d / (a > 0.0 ? a : 1e-300)));
        }
    }
    *closest = p0 + edge0 * s + edge1 * t;
    return (*closest - point).Length();
}

// Whether a point is inside a closed triangulated surface.
//
// THE LAST RESORT IS NOT A GUESS. This counted ray crossings in three
// directions and returned "outside" when all three passed too near an
// edge to be trusted -- which on a structured mesh is not the rare case
// at all: a box's triangles are coplanar in sheets and share edges in
// rows, so a fixed direction grazes something almost every time. Whole
// interiors were being called outside, and a wrong answer on one thin
// element tears the boundary: the symptom is an edge belonging to four
// boundary faces.
//
// So there are more directions, and beneath them a test that cannot
// graze: the nearest point on the surface, and which side of its
// triangle the point is on. That is exact for a closed surface except
// when the nearest point is on an edge between two triangles facing
// different ways, which is why it is the fallback and not the method.
bool InsideSurface(const std::vector<Vec3d> &nodes, const std::vector<SurfaceTriangle> &triangles,
                   const Vec3d &point) {
    // Deliberately unrelated to each other and to any axis: a direction
    // that grazes one sheet of coplanar triangles should not be replaced
    // by another that grazes the same sheet.
    static const Vec3d directions[8] = {
        {0.5773502691896258, 0.5773502691896258, 0.5773502691896258},
        {0.2672612419124244, 0.5345224838248488, 0.8017837257372732},
        {0.8017837257372732, -0.5345224838248488, 0.2672612419124244},
        {-0.4242640687119285, 0.8081220356417685, -0.4082482904638631},
        {0.1543033499620919, -0.3086066999241838, 0.9386813331646488},
        {-0.7071067811865476, -0.1961161351381841, 0.6793662204867575},
        {0.3692744729379982, 0.8616404368553292, -0.3477574107276296},
        {-0.2182178902359924, -0.4364357804719848, -0.8728715609439696}};
    for (const Vec3d &direction : directions) {
        int crossings = 0;
        bool grazed = false;
        for (const SurfaceTriangle &t : triangles) {
            const Vec3d &p0 = nodes[static_cast<std::size_t>(t.a)];
            const Vec3d &p1 = nodes[static_cast<std::size_t>(t.b)];
            const Vec3d &p2 = nodes[static_cast<std::size_t>(t.c)];
            const Vec3d edge1 = p1 - p0;
            const Vec3d edge2 = p2 - p0;
            const Vec3d h = direction.Cross(edge2);
            const double determinant = edge1.Dot(h);
            if (std::fabs(determinant) < 1e-14) continue;
            const double inverse = 1.0 / determinant;
            const Vec3d s = point - p0;
            const double u = inverse * s.Dot(h);
            if (u < -1e-10 || u > 1.0 + 1e-10) continue;
            const Vec3d q = s.Cross(edge1);
            const double v = inverse * direction.Dot(q);
            if (v < -1e-10 || u + v > 1.0 + 1e-10) continue;
            const double distance = inverse * edge2.Dot(q);
            if (distance <= 1e-12) continue;
            // Too close to an edge or a vertex for the parity to be
            // trusted: try another direction rather than guess.
            if (u < 1e-8 || v < 1e-8 || u + v > 1.0 - 1e-8) {
                grazed = true;
                break;
            }
            ++crossings;
        }
        if (!grazed) return (crossings % 2) == 1;
    }
    double nearest = std::numeric_limits<double>::infinity();
    Vec3d normal{0, 0, 1};
    Vec3d at = point;
    for (const SurfaceTriangle &t : triangles) {
        const Vec3d &p0 = nodes[static_cast<std::size_t>(t.a)];
        const Vec3d &p1 = nodes[static_cast<std::size_t>(t.b)];
        const Vec3d &p2 = nodes[static_cast<std::size_t>(t.c)];
        Vec3d closest;
        const double distance = DistanceToTriangle(point, p0, p1, p2, &closest);
        if (distance >= nearest) continue;
        nearest = distance;
        at = closest;
        normal = (p1 - p0).Cross(p2 - p0);
    }
    return normal.Dot(point - at) < 0.0;
}

// The boundary is what the elements leave exposed: every face used by
// exactly one of them. Watertight by construction -- a face used by two
// is interior, and there is no third possibility in a tetrahedralisation.
// Each face keeps the CAD face its nodes came from, so that Part H.2 can
// still attach loads and constraints per face.
void RebuildBoundary(VolumeMesh *out, const std::vector<SurfaceTriangle> &from, int surface_nodes) {
    out->boundary.clear();
    std::map<std::array<int, 3>, std::pair<int, SurfaceTriangle>> exposed;
    for (const Tetrahedron &t : out->tets) {
        const int v[4] = {t.a, t.b, t.c, t.d};
        for (int which = 0; which < 4; ++which) {
            // Wound outwards: the omitted vertex is the inside.
            std::array<int, 3> face{};
            int at = 0;
            for (int k = 0; k < 4; ++k) {
                if (k != which) face[static_cast<std::size_t>(at++)] = v[k];
            }
            const Vec3d &a = out->nodes[static_cast<std::size_t>(face[0])];
            const Vec3d &b = out->nodes[static_cast<std::size_t>(face[1])];
            const Vec3d &c = out->nodes[static_cast<std::size_t>(face[2])];
            if ((b - a).Cross(c - a).Dot(out->nodes[static_cast<std::size_t>(v[which])] - a) > 0.0) {
                std::swap(face[1], face[2]);
            }
            std::array<int, 3> key = face;
            std::sort(key.begin(), key.end());
            auto &slot = exposed[key];
            ++slot.first;
            slot.second = SurfaceTriangle{face[0], face[1], face[2], cad::EntityId{}};
        }
    }
    std::map<std::array<int, 3>, cad::EntityId> named;
    for (const SurfaceTriangle &t : from) {
        std::array<int, 3> key{t.a, t.b, t.c};
        std::sort(key.begin(), key.end());
        named[key] = t.face;
    }
    for (const auto &entry : exposed) {
        if (entry.second.first != 1) continue;
        SurfaceTriangle face = entry.second.second;
        const auto found = named.find(entry.first);
        if (found != named.end()) {
            face.face = found->second;
        } else if (face.a < surface_nodes) {
            face.face = out->provenance[static_cast<std::size_t>(face.a)].face;
        }
        out->boundary.push_back(face);
    }
}

// --- G.4: sliver removal by flipping ------------------------------------
//
// A SLIVER IS NOT DELETED, IT IS FLIPPED AWAY. Four nearly coplanar
// nodes make an element with almost no volume and a dihedral angle near
// zero and another near 180, which a solver's shape-function
// derivatives divide by. It cannot be deleted -- that leaves a hole --
// and it cannot be smoothed away either, because its four nodes are
// where they should be; it is the *connectivity* that is wrong. So the
// region it sits in is re-triangulated, which changes no node and moves
// no boundary.
//
// The three flips between them cover every re-triangulation of a small
// region that keeps its outer faces:
//
//   2-3  two elements sharing a face become three sharing a new edge
//   3-2  three elements sharing an edge become two sharing a new face
//   4-4  four elements around an edge become four around another; the
//        region is an octahedron and this picks a different one of its
//        three diagonals
//
// Every one of them is refused unless the pieces put back fill exactly
// the same space as the pieces taken out -- the volumes are summed and
// compared, which catches the non-convex cases where the flip is simply
// not available -- and unless the worst element it touches comes out
// better than it went in. Nothing here can make a mesh worse; the most
// it can do is nothing.

// The smallest of a tetrahedron's six dihedral angles, in degrees. The
// one number that says "sliver": a bad element has a tiny one, and the
// huge one that must accompany it says nothing extra.
double WorstDihedral(const Vec3d &a, const Vec3d &b, const Vec3d &c, const Vec3d &d) {
    const Vec3d faces[4] = {(c - b).Cross(d - b), (d - a).Cross(c - a), (b - a).Cross(d - a),
                            (c - a).Cross(b - a)};
    double worst = 180.0;
    for (int i = 0; i < 4; ++i) {
        for (int j = i + 1; j < 4; ++j) {
            const double na = faces[i].Length();
            const double nb = faces[j].Length();
            if (!(na > 0.0 && nb > 0.0)) return 0.0;
            const double cosine = faces[i].Dot(faces[j]) / (na * nb);
            const double angle =
                180.0 - std::acos(std::max(-1.0, std::min(1.0, cosine))) * 180.0 / cad::kPi;
            worst = std::min(worst, angle);
        }
    }
    return worst;
}

// A tetrahedron wound so that its volume is positive, by the exact
// predicate rather than by the volume formula -- for the reason recorded
// where the mesher winds its elements.
Tetrahedron Wound(const std::vector<Vec3d> &nodes, int a, int b, int c, int d) {
    Tetrahedron out{a, b, c, d};
    if (cad::Orient3D(Coord(nodes, a).data(), Coord(nodes, b).data(), Coord(nodes, c).data(),
                      Coord(nodes, d).data()) > 0) {
        std::swap(out.c, out.d);
    }
    return out;
}

double QualityOf(const std::vector<Vec3d> &nodes, const Tetrahedron &t) {
    return WorstDihedral(nodes[static_cast<std::size_t>(t.a)], nodes[static_cast<std::size_t>(t.b)],
                         nodes[static_cast<std::size_t>(t.c)], nodes[static_cast<std::size_t>(t.d)]);
}

double VolumeOf(const std::vector<Vec3d> &nodes, const Tetrahedron &t) {
    return TetVolume(nodes[static_cast<std::size_t>(t.a)], nodes[static_cast<std::size_t>(t.b)],
                     nodes[static_cast<std::size_t>(t.c)], nodes[static_cast<std::size_t>(t.d)]);
}

// Accepts a proposed re-triangulation if it fills the same space and the
// worst of it is better than the worst of what it replaces.
bool FlipIsAnImprovement(const std::vector<Vec3d> &nodes, const std::vector<Tetrahedron> &before,
                         const std::vector<Tetrahedron> &after) {
    double old_volume = 0.0;
    double worst_before = 180.0;
    for (const Tetrahedron &t : before) {
        old_volume += std::fabs(VolumeOf(nodes, t));
        worst_before = std::min(worst_before, QualityOf(nodes, t));
    }
    double new_volume = 0.0;
    double worst_after = 180.0;
    for (const Tetrahedron &t : after) {
        const int winding =
            cad::Orient3D(Coord(nodes, t.a).data(), Coord(nodes, t.b).data(),
                          Coord(nodes, t.c).data(), Coord(nodes, t.d).data());
        // A flat piece means the region is not convex enough for this
        // flip -- the pieces would overlap rather than tile.
        if (winding == 0) return false;
        new_volume += std::fabs(VolumeOf(nodes, t));
        worst_after = std::min(worst_after, QualityOf(nodes, t));
    }
    // THE VOLUMES HAVE TO MATCH. This is what makes the flip safe
    // without a separate convexity test: pieces that overlap or leave a
    // gap do not add up, and a flip that does not add up would put a
    // hole in the mesh that only the volume check at the end would find.
    if (std::fabs(new_volume - old_volume) > old_volume * 1e-9) return false;
    return worst_after > worst_before + 1e-9;
}

std::array<int, 2> EdgeKey(int a, int b) {
    return a < b ? std::array<int, 2>{a, b} : std::array<int, 2>{b, a};
}

// Re-triangulates the region around bad elements, in passes. Returns how
// many flips were made.
int ImproveByFlipping(const std::vector<Vec3d> &nodes, std::vector<Tetrahedron> *tets, int passes,
                      double below_degrees) {
    int flips = 0;
    for (int pass = 0; pass < passes; ++pass) {
        std::vector<bool> alive(tets->size(), true);
        std::map<std::array<int, 3>, std::vector<int>> by_face;
        std::map<std::array<int, 2>, std::vector<int>> by_edge;
        for (std::size_t i = 0; i < tets->size(); ++i) {
            const Tetrahedron &t = (*tets)[i];
            const int v[4] = {t.a, t.b, t.c, t.d};
            for (int which = 0; which < 4; ++which) {
                std::array<int, 3> face{};
                int at = 0;
                for (int k = 0; k < 4; ++k) {
                    if (k != which) face[static_cast<std::size_t>(at++)] = v[k];
                }
                std::sort(face.begin(), face.end());
                by_face[face].push_back(static_cast<int>(i));
            }
            for (int i0 = 0; i0 < 4; ++i0) {
                for (int i1 = i0 + 1; i1 < 4; ++i1) {
                    by_edge[EdgeKey(v[i0], v[i1])].push_back(static_cast<int>(i));
                }
            }
        }

        // Worst first: a mesh is judged by its worst element, so that is
        // where a pass's effort belongs.
        std::vector<int> order;
        for (std::size_t i = 0; i < tets->size(); ++i) {
            if (QualityOf(nodes, (*tets)[i]) < below_degrees) order.push_back(static_cast<int>(i));
        }
        std::sort(order.begin(), order.end(), [&](int x, int y) {
            return QualityOf(nodes, (*tets)[static_cast<std::size_t>(x)]) <
                   QualityOf(nodes, (*tets)[static_cast<std::size_t>(y)]);
        });
        if (order.empty()) break;

        std::vector<Tetrahedron> added;
        auto apply = [&](const std::vector<int> &replaced, const std::vector<Tetrahedron> &with) {
            for (const int which : replaced) alive[static_cast<std::size_t>(which)] = false;
            for (const Tetrahedron &t : with) added.push_back(t);
            ++flips;
        };
        auto usable = [&](const std::vector<int> &which) {
            for (const int i : which) {
                if (!alive[static_cast<std::size_t>(i)]) return false;
            }
            return true;
        };
        auto gather = [&](const std::vector<int> &which) {
            std::vector<Tetrahedron> out;
            for (const int i : which) out.push_back((*tets)[static_cast<std::size_t>(i)]);
            return out;
        };

        for (const int seed : order) {
            if (!alive[static_cast<std::size_t>(seed)]) continue;
            const Tetrahedron &t = (*tets)[static_cast<std::size_t>(seed)];
            const int v[4] = {t.a, t.b, t.c, t.d};
            bool done = false;

            // 2-3, on each of its four faces.
            for (int which = 0; which < 4 && !done; ++which) {
                std::array<int, 3> face{};
                int at = 0;
                for (int k = 0; k < 4; ++k) {
                    if (k != which) face[static_cast<std::size_t>(at++)] = v[k];
                }
                std::sort(face.begin(), face.end());
                const std::vector<int> &pair = by_face[face];
                // One element means the face is on the boundary, which a
                // mesher may not alter.
                if (pair.size() != 2 || !usable(pair)) continue;
                int apex[2] = {-1, -1};
                for (int side = 0; side < 2; ++side) {
                    const Tetrahedron &other = (*tets)[static_cast<std::size_t>(pair[static_cast<std::size_t>(side)])];
                    const int w[4] = {other.a, other.b, other.c, other.d};
                    for (const int node : w) {
                        if (node != face[0] && node != face[1] && node != face[2]) {
                            apex[side] = node;
                        }
                    }
                }
                if (apex[0] < 0 || apex[1] < 0 || apex[0] == apex[1]) continue;
                const std::vector<Tetrahedron> after = {
                    Wound(nodes, face[0], face[1], apex[0], apex[1]),
                    Wound(nodes, face[1], face[2], apex[0], apex[1]),
                    Wound(nodes, face[2], face[0], apex[0], apex[1])};
                if (!FlipIsAnImprovement(nodes, gather(pair), after)) continue;
                apply(pair, after);
                done = true;
            }

            // 3-2 and 4-4, on each of its six edges.
            for (int i0 = 0; i0 < 4 && !done; ++i0) {
                for (int i1 = i0 + 1; i1 < 4 && !done; ++i1) {
                    const std::array<int, 2> edge = EdgeKey(v[i0], v[i1]);
                    const std::vector<int> &ring = by_edge[edge];
                    if (!usable(ring)) continue;
                    // The vertices around the edge, one per element.
                    std::vector<int> around;
                    for (const int i : ring) {
                        const Tetrahedron &other = (*tets)[static_cast<std::size_t>(i)];
                        const int w[4] = {other.a, other.b, other.c, other.d};
                        for (const int node : w) {
                            if (node == edge[0] || node == edge[1]) continue;
                            if (std::find(around.begin(), around.end(), node) == around.end()) {
                                around.push_back(node);
                            }
                        }
                    }
                    if (ring.size() == 3 && around.size() == 3) {
                        const std::vector<Tetrahedron> after = {
                            Wound(nodes, around[0], around[1], around[2], edge[0]),
                            Wound(nodes, around[0], around[1], around[2], edge[1])};
                        if (!FlipIsAnImprovement(nodes, gather(ring), after)) continue;
                        apply(ring, after);
                        done = true;
                        continue;
                    }
                    if (ring.size() != 4 || around.size() != 4) continue;
                    // The ring in order, so that consecutive vertices
                    // share an element with the edge. Without that the
                    // quadrilateral's diagonals are not the diagonals.
                    std::vector<int> cycle{around[0]};
                    for (int step = 0; step < 3; ++step) {
                        const int from = cycle.back();
                        int next = -1;
                        for (const int i : ring) {
                            const Tetrahedron &other = (*tets)[static_cast<std::size_t>(i)];
                            const int w[4] = {other.a, other.b, other.c, other.d};
                            bool has_from = false;
                            int candidate = -1;
                            for (const int node : w) {
                                if (node == edge[0] || node == edge[1]) continue;
                                if (node == from) has_from = true;
                                else candidate = node;
                            }
                            if (!has_from || candidate < 0) continue;
                            if (std::find(cycle.begin(), cycle.end(), candidate) != cycle.end()) {
                                continue;
                            }
                            next = candidate;
                            break;
                        }
                        if (next < 0) break;
                        cycle.push_back(next);
                    }
                    if (cycle.size() != 4) continue;
                    // Either diagonal of the quadrilateral, in place of
                    // the edge itself.
                    for (int diagonal = 0; diagonal < 2 && !done; ++diagonal) {
                        const int p0 = cycle[static_cast<std::size_t>(diagonal)];
                        const int p1 = cycle[static_cast<std::size_t>(diagonal + 1)];
                        const int p2 = cycle[static_cast<std::size_t>(diagonal + 2)];
                        const int p3 = cycle[static_cast<std::size_t>((diagonal + 3) % 4)];
                        const std::vector<Tetrahedron> after = {
                            Wound(nodes, p0, p1, p2, edge[0]), Wound(nodes, p0, p1, p2, edge[1]),
                            Wound(nodes, p0, p2, p3, edge[0]), Wound(nodes, p0, p2, p3, edge[1])};
                        if (!FlipIsAnImprovement(nodes, gather(ring), after)) continue;
                        apply(ring, after);
                        done = true;
                    }
                }
            }
        }

        if (added.empty()) break;
        std::vector<Tetrahedron> kept;
        for (std::size_t i = 0; i < tets->size(); ++i) {
            if (alive[i]) kept.push_back((*tets)[i]);
        }
        for (const Tetrahedron &t : added) kept.push_back(t);
        *tets = kept;
    }
    return flips;
}

}  // namespace

void TetrahedralisePointsImpl(const std::vector<Vec3d> &points, std::vector<Tetrahedron> *out) {
    std::vector<Tet> tets;
    Tetrahedralise(points, &tets);
    out->clear();
    for (const Tet &t : tets) {
        Tetrahedron keep{t.a, t.b, t.c, t.d};
        if (TetVolume(points[static_cast<std::size_t>(t.a)], points[static_cast<std::size_t>(t.b)],
                      points[static_cast<std::size_t>(t.c)],
                      points[static_cast<std::size_t>(t.d)]) < 0.0) {
            std::swap(keep.c, keep.d);
        }
        out->push_back(keep);
    }
}

void TetrahedralisePoints(const std::vector<cad::Vec3d> &points, std::vector<Tetrahedron> *out) {
    TetrahedralisePointsImpl(points, out);
}

MeshQuality MeasureQuality(const VolumeMesh &mesh) {
    MeshQuality out;
    out.min_dihedral = 180.0;
    out.min_scaled_jacobian = 1.0;
    out.min_volume = std::numeric_limits<double>::infinity();
    out.worst_aspect = 0.0;
    // A HEX MESH IS JUDGED BY ITS JACOBIAN, NOT BY ITS ANGLES. A
    // hexahedron has twenty-four face angles and no single worst one
    // that means anything, so the dihedral summary is left alone for
    // one and the scaled Jacobian carries the verdict. A mesh holds
    // hexahedra or tetrahedra, never both, so the two do not have to be
    // reconciled -- but saying so here is cheaper than finding out later.
    if (!mesh.hexes.empty()) {
        out.min_dihedral = 0.0;
        out.max_dihedral = 0.0;
        out.min_volume = std::numeric_limits<double>::infinity();
        for (const Hexahedron &hex : mesh.hexes) {
            const double scaled = HexScaledJacobian(mesh.nodes, hex);
            const double volume = HexVolume(mesh.nodes, hex);
            out.min_scaled_jacobian = std::min(out.min_scaled_jacobian, scaled);
            out.min_volume = std::min(out.min_volume, volume);
            if (!(scaled > 0.0) || !(volume > 0.0)) ++out.inverted;
            // The same ten-degree bins as the tetrahedra use, filled from
            // the angle whose sine is the scaled Jacobian, so that the
            // two meshes can be compared on one histogram at all.
            const double equivalent =
                std::asin(std::max(0.0, std::min(1.0, scaled))) * 180.0 / cad::kPi;
            const int bin = std::max(0, std::min(17, static_cast<int>(equivalent / 10.0)));
            ++out.dihedral_histogram[static_cast<std::size_t>(bin)];
            if (scaled < 0.1) ++out.slivers;
        }
        if (!std::isfinite(out.min_volume)) out.min_volume = 0.0;
        if (mesh.tets.empty()) return out;
    }
    if (mesh.tets.empty()) {
        out.min_dihedral = 0.0;
        out.min_volume = 0.0;
        out.min_scaled_jacobian = 0.0;
        return out;
    }
    for (const Tetrahedron &t : mesh.tets) {
        const Vec3d &a = mesh.nodes[static_cast<std::size_t>(t.a)];
        const Vec3d &b = mesh.nodes[static_cast<std::size_t>(t.b)];
        const Vec3d &c = mesh.nodes[static_cast<std::size_t>(t.c)];
        const Vec3d &d = mesh.nodes[static_cast<std::size_t>(t.d)];
        const double volume = TetVolume(a, b, c, d);
        out.min_volume = std::min(out.min_volume, volume);
        if (volume <= 0.0) ++out.inverted;

        // The six dihedral angles, from the four face normals. The
        // minimum and maximum together are the standard summary of a
        // tetrahedron's shape: a sliver has a tiny one and a huge one at
        // once, which neither on its own would show.
        const Vec3d faces[4] = {(c - b).Cross(d - b), (d - a).Cross(c - a), (b - a).Cross(d - a),
                                (c - a).Cross(b - a)};
        double worst_here = 180.0;
        for (int i = 0; i < 4; ++i) {
            for (int j = i + 1; j < 4; ++j) {
                const double na = faces[i].Length();
                const double nb = faces[j].Length();
                if (!(na > 0.0 && nb > 0.0)) continue;
                const double cosine = faces[i].Dot(faces[j]) / (na * nb);
                const double angle =
                    180.0 - std::acos(std::max(-1.0, std::min(1.0, cosine))) * 180.0 / cad::kPi;
                out.min_dihedral = std::min(out.min_dihedral, angle);
                out.max_dihedral = std::max(out.max_dihedral, angle);
                worst_here = std::min(worst_here, angle);
            }
        }
        const int bin = std::max(0, std::min(17, static_cast<int>(worst_here / 10.0)));
        ++out.dihedral_histogram[static_cast<std::size_t>(bin)];
        if (worst_here < 5.0) ++out.slivers;

        // Aspect ratio as the longest edge over the inradius, normalised
        // so that a regular tetrahedron scores 1.
        const double edges[6] = {(b - a).Length(), (c - a).Length(), (d - a).Length(),
                                 (c - b).Length(), (d - b).Length(), (d - c).Length()};
        double longest = 0.0;
        for (double e : edges) longest = std::max(longest, e);
        double area = 0.0;
        for (const Vec3d &face : faces) area += 0.5 * face.Length();
        const double inradius = area > 0.0 ? 3.0 * std::fabs(volume) / area : 0.0;
        if (inradius > 0.0) {
            out.worst_aspect = std::max(out.worst_aspect, longest / (inradius * 2.0 * std::sqrt(6.0)));
        } else {
            out.worst_aspect = std::numeric_limits<double>::infinity();
        }
        // The scaled Jacobian: the determinant normalised by the edge
        // lengths, which is what every mesh checker reports and what a
        // solver's element routines actually care about.
        double scaled = 0.0;
        if (edges[0] > 0.0 && edges[1] > 0.0 && edges[2] > 0.0) {
            scaled = 6.0 * volume * std::sqrt(2.0) / (edges[0] * edges[1] * edges[2]);
        }
        out.min_scaled_jacobian = std::min(out.min_scaled_jacobian, scaled);
    }
    if (!std::isfinite(out.min_volume)) out.min_volume = 0.0;
    return out;
}

bool CheckMesh(const VolumeMesh &mesh, std::string *error) {
    error->clear();
    if (!CheckCurvature(mesh, error)) return false;
    if (!mesh.hexes.empty() && !CheckHexes(mesh, error)) return false;
    if (mesh.tets.empty()) {
        // A swept mesh is all hexahedra and has no tetrahedra at all,
        // which is not the same thing as having no elements.
        if (!mesh.hexes.empty()) return true;
        *error = "the mesh has no elements";
        return false;
    }
    for (std::size_t i = 0; i < mesh.tets.size(); ++i) {
        const Tetrahedron &t = mesh.tets[i];
        const int nodes[4] = {t.a, t.b, t.c, t.d};
        for (int n : nodes) {
            if (n < 0 || n >= mesh.NodeCount()) {
                *error = "element " + std::to_string(i) + " refers to node " + std::to_string(n) +
                         ", which does not exist";
                return false;
            }
        }
        // Asked of the exact predicate, so that an element too flat for
        // the volume formula to sign reliably gets the same answer here
        // as it did when the mesher wound it.
        if (cad::Orient3D(Coord(mesh.nodes, t.a).data(), Coord(mesh.nodes, t.b).data(),
                          Coord(mesh.nodes, t.c).data(), Coord(mesh.nodes, t.d).data()) >= 0) {
            *error = "element " + std::to_string(i) + " has zero or negative volume";
            return false;
        }
    }
    // The boundary: every face used once is a boundary face, and those
    // have to form a closed surface.
    std::map<std::array<int, 3>, int> faces;
    for (const Tetrahedron &t : mesh.tets) {
        const std::array<std::array<int, 3>, 4> four = {
            std::array<int, 3>{t.b, t.c, t.d}, std::array<int, 3>{t.a, t.d, t.c},
            std::array<int, 3>{t.a, t.b, t.d}, std::array<int, 3>{t.a, t.c, t.b}};
        for (std::array<int, 3> face : four) {
            std::sort(face.begin(), face.end());
            ++faces[face];
        }
    }
    std::map<std::pair<int, int>, int> boundary_edges;
    int boundary_faces = 0;
    for (const auto &entry : faces) {
        if (entry.second > 2) {
            *error = "a face is shared by " + std::to_string(entry.second) +
                     " elements, which is not a tetrahedral mesh";
            return false;
        }
        if (entry.second != 1) continue;
        ++boundary_faces;
        for (int k = 0; k < 3; ++k) {
            int u = entry.first[static_cast<std::size_t>(k)];
            int v = entry.first[static_cast<std::size_t>((k + 1) % 3)];
            if (u > v) std::swap(u, v);
            ++boundary_edges[{u, v}];
        }
    }
    if (boundary_faces == 0) {
        *error = "the mesh has no boundary at all";
        return false;
    }
    for (const auto &entry : boundary_edges) {
        if (entry.second == 2) continue;
        *error = "the boundary is not closed: an edge is used by " + std::to_string(entry.second) +
                 " boundary faces";
        return false;
    }
    return true;
}

bool ToAnalysisModel(const VolumeMesh &mesh, const Material &material, Model *out,
                     std::string *error) {
    error->clear();
    *out = Model{};
    if (mesh.tets.empty()) {
        *error = "the mesh has no elements";
        return false;
    }
    // fem_model.h knows Hex8 and nothing else so far (Part H.1 is the
    // element library). Saying so is better than writing tetrahedra into
    // a field that means hexahedra.
    *error = "this mesh is tetrahedral and the analysis model so far carries only Hex8 elements "
             "(Part H.1 is the element library); " +
             std::to_string(mesh.tets.size()) + " elements were not converted";
    out->nodes = mesh.nodes;
    out->material = material;
    return false;
}

bool MeshBody(const cad::Model &model, const std::vector<cad::EntityId> &bodies,
              const VolumeMeshOptions &options, VolumeMesh *out, MeshReport *report) {
    *out = VolumeMesh{};
    *report = MeshReport{};

    SizingField sizing;
    if (!sizing.Build(model, bodies, options.surface.sizing, &report->error)) return false;

    SurfaceMesh surface;
    if (!MeshSurface(model, bodies, options.surface, sizing, &surface, report)) return false;
    if (!surface.IsClosed()) {
        report->error = "the surface mesh is not closed, so there is no volume to fill";
        return false;
    }

    out->nodes = surface.nodes;
    out->provenance = surface.provenance;
    out->boundary = surface.triangles;

    // --- Interior points ---------------------------------------------------
    //
    // A mesh whose only nodes are on the boundary is all boundary: every
    // element touches the surface, and a thick part gets elements as long
    // as it is thick. Laid down on a lattice spaced by the sizing field
    // and kept where they are inside, which is the cheap half of what a
    // proper interior insertion does and enough for a graded field.
    if (options.interior_points) {
        cad::Box3d extent;
        for (const Vec3d &p : surface.nodes) extent.Expand(p);
        const double step = std::max(sizing.MinSize(), sizing.TargetSize()) * 0.85;
        const int nx = std::max(1, static_cast<int>(extent.x.Width() / step));
        const int ny = std::max(1, static_cast<int>(extent.y.Width() / step));
        const int nz = std::max(1, static_cast<int>(extent.z.Width() / step));
        for (int i = 1; i < nx; ++i) {
            for (int j = 1; j < ny; ++j) {
                for (int k = 1; k < nz; ++k) {
                    Vec3d at{extent.x.lo + extent.x.Width() * i / nx,
                             extent.y.lo + extent.y.Width() * j / ny,
                             extent.z.lo + extent.z.Width() * k / nz};
                    // Offset by a fraction of the step so the lattice
                    // does not line up with the boundary, which would put
                    // interior points exactly on faces.
                    at = at + Vec3d{step * 0.123, step * 0.211, step * 0.317};
                    const double wanted = sizing.At(at);
                    bool crowded = false;
                    for (const Vec3d &node : surface.nodes) {
                        if ((node - at).LengthSquared() < wanted * wanted * 0.36) {
                            crowded = true;
                            break;
                        }
                    }
                    if (crowded) continue;
                    if (!InsideSurface(surface.nodes, surface.triangles, at)) continue;
                    out->nodes.push_back(at);
                    out->provenance.push_back({});
                }
            }
        }
    }

    // --- Delaunay, then the boundary it chose -----------------------------
    //
    // A Delaunay tetrahedralisation of the surface's nodes need not
    // contain the surface mesher's triangles, and on a planar face it
    // usually does not: the nodes there are exactly coplanar, so which
    // diagonal each quadrilateral among them gets is a tie, and the two
    // meshers break it independently. That is not a defect in either of
    // them and it is not worth chasing.
    //
    // IT WAS CHASED FIRST, WITH STEINER POINTS, AND IT DIVERGED: five
    // thousand of them left nine thousand triangles still missing.
    // Splitting a coplanar triangle at its centre adds another coplanar
    // node, which is another tie, which the tetrahedraliser breaks its
    // own way again. The loop was adding work to a problem it could not
    // reduce.
    //
    // So the surface mesher's triangulation is not the target. The nodes
    // are. A different triangulation of the same nodes spans the same
    // polygon on a planar face and interpolates the same points on a
    // curved one, so the boundary the tetrahedralisation chooses is the
    // boundary -- read off the elements that were kept, which makes it
    // watertight by construction rather than by recovery. What the
    // surface mesher decided still fixes every node position and so the
    // shape; only the diagonals are the tetrahedraliser's.
    const int surface_nodes = static_cast<int>(surface.nodes.size());
    std::vector<Tet> tets;
    Tetrahedralise(out->nodes, &tets);
    if (tets.empty()) {
        report->error = "the tetrahedralisation produced nothing";
        return false;
    }
    report->steiner_points = 0;
    // --- Inside or outside ---------------------------------------------
    //
    // One test per element, against the surface mesh. The obvious thing,
    // and it is here only after the clever thing was tried and was
    // wrong.
    //
    // The clever thing was to flood: build a barrier out of the faces
    // that lie on the body, let the elements fall into regions that do
    // not cross it, and decide each region once. That is much less work
    // and it fails on exactly the bodies it was meant for. A barrier is
    // built by asking whether a face lies on the body, and where the
    // boundary turns a reflex corner the honest answer for every
    // candidate face is no, because the tetrahedralisation cuts the
    // corner rather than turning it. The barrier then has a hole,
    // inside and outside join into one region, and the single test that
    // decides it is made deep in the material and says inside. On an
    // L-shaped block that kept half the notch: 224 of volume where the
    // body has 192. Sampling the region at several elements does not
    // help -- its biggest ones are all in the material and all agree.
    //
    // Per element there is no hole to leak through. It costs a ray test
    // each, which is the price of an answer that does not depend on a
    // barrier being complete, and the ray test is now reliable enough to
    // be asked near the surface, which is what made this affordable.
    std::vector<bool> keep(tets.size(), false);
    for (std::size_t i = 0; i < tets.size(); ++i) {
        const Tet &t = tets[i];
        const Vec3d centroid = (out->nodes[static_cast<std::size_t>(t.a)] +
                                out->nodes[static_cast<std::size_t>(t.b)] +
                                out->nodes[static_cast<std::size_t>(t.c)] +
                                out->nodes[static_cast<std::size_t>(t.d)]) *
                               0.25;
        keep[i] = InsideSurface(out->nodes, surface.triangles, centroid);
    }
    for (std::size_t i = 0; i < tets.size(); ++i) {
        if (!keep[i]) continue;
        // WOUND BY THE EXACT PREDICATE, NOT BY THE VOLUME FORMULA. For a
        // nearly flat element the two windings' volumes are not exact
        // negations of each other -- they are different sums of the same
        // rounded products -- so a tetrahedron measuring -1e-19 can still
        // measure negative after being turned over, and the mesh then
        // fails its own check. Orient3D has no such flat spot. Its sign
        // is the opposite way round from the volume formula's, so a
        // positive volume is a negative orientation.
        const int winding = cad::Orient3D(Coord(out->nodes, tets[i].a).data(),
                                          Coord(out->nodes, tets[i].b).data(),
                                          Coord(out->nodes, tets[i].c).data(),
                                          Coord(out->nodes, tets[i].d).data());
        if (winding == 0) continue;
        Tetrahedron element{tets[i].a, tets[i].b, tets[i].c, tets[i].d};
        if (winding > 0) std::swap(element.c, element.d);
        out->tets.push_back(element);
    }
    if (out->tets.empty()) {
        report->error = "no region of the triangulation was found to be inside the body";
        return false;
    }
    RebuildBoundary(out, surface.triangles, surface_nodes);

    // --- Quality (Part G.4) -------------------------------------------------
    //
    // Flip, then smooth, then flip again. The two do different jobs and
    // each makes work for the other: flipping fixes an element whose
    // nodes are in the right places and whose connectivity is not,
    // smoothing fixes nodes that are in the wrong places, and a sliver
    // that neither could touch on its own often gives way once the other
    // has been past. The second pass is where most of the cylinder's
    // slivers go.
    // REPEATED UNTIL IT STOPS HELPING, not run once. Flipping and
    // smoothing feed each other -- a flip puts nodes somewhere smoothing
    // can use, and a smoothed node opens a flip that was not available --
    // so one round of each leaves work that a second round finds. One
    // round was enough for a block and not for the graded meshes an
    // adaptive cycle asks for: the L-bracket's second cycle came back
    // with a 1.9-degree sliver and stopped the loop.
    //
    // Bounded, and stopped as soon as the mesh is good enough or has
    // stopped improving, so the common case still costs one round. There
    // is no convergence guarantee here and there does not need to be:
    // every individual move is refused unless it makes the worst element
    // it touches better, so the worst angle is monotone and the only
    // question is when to stop paying for it.
    auto worst_angle = [&]() {
        double worst = 180.0;
        for (const Tetrahedron &t : out->tets) {
            worst = std::min(worst, WorstDihedral(out->nodes[static_cast<std::size_t>(t.a)],
                                                  out->nodes[static_cast<std::size_t>(t.b)],
                                                  out->nodes[static_cast<std::size_t>(t.c)],
                                                  out->nodes[static_cast<std::size_t>(t.d)]));
        }
        return worst;
    };
    for (int round = 0; round < 8; ++round) {
        const double before = worst_angle();
        if (before >= options.min_acceptable_dihedral && round > 0) break;
        if (options.remove_slivers) {
            report->flips = ImproveByFlipping(out->nodes, &out->tets, 6, 15.0);
        }
        if (options.smooth) {
            // Laplacian smoothing, of the interior nodes freely and of the
            // boundary nodes *along the geometry they came from*.
            //
            // THE EASY VERSION SMOOTHS ONLY THE INTERIOR, on the grounds that
            // a boundary node moved is a boundary changed, and that is the
            // one thing a mesher may not do. It is also useless on exactly
            // the meshes that need it most: a thin slab two elements thick
            // has *no* interior nodes at all, so the whole pass does nothing,
            // and a sliver there is left for the flipping to fix alone --
            // which it cannot, because a sliver whose four nodes are all on
            // the boundary has no interior face to flip across. A 1.0 x 0.25
            // x 0.25 block asked for 1.5 elements across its thickness came
            // out with a worst dihedral angle of 4.3 degrees and was refused
            // by the mesher's own quality gate.
            //
            // The boundary need not be frozen, only *respected*. A node that
            // came from a face may slide anywhere on that face; a node from
            // an edge may slide along that edge; a node at a vertex may not
            // move at all. Part G.2 already recorded which of those each node
            // is, and the exact surface is still here to re-evaluate -- so
            // the node lands on the true geometry rather than on the chord it
            // was sitting on, which is if anything a slight improvement to
            // the boundary rather than a change to it. This is the same
            // argument as G.5's mid-side projection and the same payoff for
            // having kept the B-rep.
            //
            // AND IT IS A REPAIR, NOT AN IMPROVEMENT: only nodes touching an
            // element the mesh would otherwise *refuse* are allowed to move.
            // Letting every boundary node slide is better on the quality
            // numbers and wrong in a way that took a test failure to see.
            // Boundary conditions are routinely attached by position -- "the
            // nodes on x = 0", a symmetry plane -- and a node sliding
            // tangentially along a cylinder leaves that plane while staying
            // perfectly on the geometry. The cylinder under external pressure
            // in mep-fem-static-test lost its symmetry restraint exactly that
            // way and came back as a singular matrix. The mesher cannot know
            // which positions a caller cares about, so the answer is to
            // disturb the surface only where the alternative is no mesh at
            // all.
            std::vector<bool> on_boundary(out->nodes.size(), false);
            for (const SurfaceTriangle &t : out->boundary) {
                on_boundary[static_cast<std::size_t>(t.a)] = true;
                on_boundary[static_cast<std::size_t>(t.b)] = true;
                on_boundary[static_cast<std::size_t>(t.c)] = true;
            }
            // Where a boundary node is allowed to go, from Part G.2's record
            // of where it came from.
            auto slide = [&](std::size_t node, const Vec3d &towards, Vec3d *out_point) {
                if (node >= out->provenance.size()) return false;
                const SurfaceMesh::Provenance &from = out->provenance[node];
                if (from.vertex != cad::kNoEntity) return false;  // a corner cannot move
                if (from.edge != cad::kNoEntity) {
                    const cad::Edge *edge = model.GetEdge(from.edge);
                    if (edge == nullptr) return false;
                    const cad::Curve3 *curve = model.CurveAt(edge->curve);
                    if (curve == nullptr) return false;
                    double t = 0.0;
                    if (!curve->ClosestPoint(towards, &t, out_point)) return false;
                    // ClosestPoint clamps to the curve's own range, so a node
                    // pushed past the end of an edge lands on its endpoint --
                    // which is a vertex, and would weld two nodes together.
                    // Refusing the move is the right answer; the flipping
                    // pass gets another turn either way.
                    double lo = 0.0, hi = 0.0;
                    curve->Domain(&lo, &hi);
                    const double margin = (hi - lo) * 1e-3;
                    return t > lo + margin && t < hi - margin;
                }
                if (from.face == cad::kNoEntity) return false;
                const cad::Face *face = model.GetFace(from.face);
                if (face == nullptr) return false;
                const cad::Surface *surface = model.SurfaceAt(face->surface);
                if (surface == nullptr) return false;
                double u = from.u;
                double v = from.v;
                if (!surface->ClosestPoint(towards, &u, &v, out_point)) return false;
                // ON THE SURFACE IS NOT THE SAME AS ON THE FACE. A face is a
                // trimmed piece of a surface, and the closest point of the
                // whole surface may be outside the trim -- off the end of a
                // cylinder, or in the hole of an annulus. Keeping the move
                // inside the parameter box the node started in is a cheap
                // approximation of the trim, and the direction it errs in is
                // the safe one: it refuses some legal moves rather than
                // allowing an illegal one.
                double u_lo = 0.0, u_hi = 0.0, v_lo = 0.0, v_hi = 0.0;
                surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
                const double u_margin = (u_hi - u_lo) * 1e-3;
                const double v_margin = (v_hi - v_lo) * 1e-3;
                return u > u_lo + u_margin && u < u_hi - u_margin && v > v_lo + v_margin &&
                       v < v_hi - v_margin;
            };
            // The boundary nodes of elements bad enough to be refused. Rebuilt
            // each pass, because repairing one sliver can leave a different
            // one as the worst.
            auto needs_repair = [&]() {
                std::vector<bool> flagged(out->nodes.size(), false);
                for (const Tetrahedron &t : out->tets) {
                    const Vec3d &a = out->nodes[static_cast<std::size_t>(t.a)];
                    const Vec3d &b = out->nodes[static_cast<std::size_t>(t.b)];
                    const Vec3d &c = out->nodes[static_cast<std::size_t>(t.c)];
                    const Vec3d &d = out->nodes[static_cast<std::size_t>(t.d)];
                    if (WorstDihedral(a, b, c, d) >= options.min_acceptable_dihedral) continue;
                    for (const int node : {t.a, t.b, t.c, t.d}) {
                        flagged[static_cast<std::size_t>(node)] = true;
                    }
                }
                return flagged;
            };
            for (int pass = 0; pass < std::max(0, options.smoothing_passes); ++pass) {
                const std::vector<bool> repairable = needs_repair();
                std::vector<Vec3d> sum(out->nodes.size(), Vec3d{});
                std::vector<int> count(out->nodes.size(), 0);
                for (const Tetrahedron &t : out->tets) {
                    const int nodes[4] = {t.a, t.b, t.c, t.d};
                    for (int i = 0; i < 4; ++i) {
                        for (int j = 0; j < 4; ++j) {
                            if (i == j) continue;
                            sum[static_cast<std::size_t>(nodes[i])] =
                                sum[static_cast<std::size_t>(nodes[i])] +
                                out->nodes[static_cast<std::size_t>(nodes[j])];
                            ++count[static_cast<std::size_t>(nodes[i])];
                        }
                    }
                }
                for (std::size_t i = 0; i < out->nodes.size(); ++i) {
                    if (count[i] == 0) continue;
                    const Vec3d target = sum[i] * (1.0 / static_cast<double>(count[i]));
                    Vec3d moved = out->nodes[i] + (target - out->nodes[i]) * 0.5;
                    if (on_boundary[i] && (!repairable[i] || !slide(i, moved, &moved))) continue;

                    // SMART LAPLACIAN: the move is kept only if the worst
                    // element touching this node comes out better than it
                    // went in. Plain Laplacian smoothing moves a node to the
                    // average of its neighbours whether that helps or not,
                    // and on a boundary node it often does not -- it rounds
                    // out the surface triangulation at the expense of the
                    // tetrahedron behind it. Freeing the boundary without
                    // this guard fixed the thin slab (4.3 degrees to 10.6)
                    // and made four other meshes worse, one of them from
                    // 13.6 to 8.3. The guard is the same rule the flipping
                    // already follows: nothing may make the mesh worse, and
                    // the most a pass can do is nothing.
                    // Only if every element it belongs to stays valid --
                    // smoothing that inverts an element has made things
                    // worse, however much rounder the node looks.
                    const Vec3d previous = out->nodes[i];
                    auto worst_around = [&]() {
                        double worst = 180.0;
                        for (const Tetrahedron &t : out->tets) {
                            if (t.a != static_cast<int>(i) && t.b != static_cast<int>(i) &&
                                t.c != static_cast<int>(i) && t.d != static_cast<int>(i)) {
                                continue;
                            }
                            const Vec3d &a = out->nodes[static_cast<std::size_t>(t.a)];
                            const Vec3d &b = out->nodes[static_cast<std::size_t>(t.b)];
                            const Vec3d &c = out->nodes[static_cast<std::size_t>(t.c)];
                            const Vec3d &d = out->nodes[static_cast<std::size_t>(t.d)];
                            // An inverted element is worse than any angle.
                            if (TetVolume(a, b, c, d) <= 0.0) return -1.0;
                            worst = std::min(worst, WorstDihedral(a, b, c, d));
                        }
                        return worst;
                    };
                    const double was = worst_around();
                    out->nodes[i] = moved;
                    if (worst_around() <= was) out->nodes[i] = previous;
                }
            }
        }

        if (options.remove_slivers) {
            report->flips += ImproveByFlipping(out->nodes, &out->tets, 6, 15.0);
            // The boundary is read off the elements, so it is read off again
            // after they change. A flip never touches a boundary face -- the
            // 2-3 needs two elements on the face it opens and an edge that a
            // 3-2 or 4-4 closes is interior by the same count -- so this
            // rebuilds the same surface, and rebuilding it is how that stays
            // true rather than something to be remembered.
            RebuildBoundary(out, surface.triangles, surface_nodes);
        }
        const double after = worst_angle();
        report->repair_rounds = round + 1;
        if (after <= before + 1e-9) break;
    }

    const MeshQuality quality = MeasureQuality(*out);
    report->nodes = out->NodeCount();
    report->triangles = static_cast<int>(out->boundary.size());
    report->tetrahedra = out->TetCount();
    report->min_dihedral = quality.min_dihedral;
    report->max_dihedral = quality.max_dihedral;
    report->worst_aspect = quality.worst_aspect;
    report->min_scaled_jacobian = quality.min_scaled_jacobian;
    report->slivers = quality.slivers;

    std::string check_error;
    if (!CheckMesh(*out, &check_error)) {
        report->error = check_error;
        return false;
    }
    // The worst angle on the boundary, which is the first thing to look
    // at when the volume is bad.
    report->min_surface_angle = 180.0;
    for (const SurfaceTriangle &t : out->boundary) {
        const Vec3d p[3] = {out->nodes[static_cast<std::size_t>(t.a)],
                            out->nodes[static_cast<std::size_t>(t.b)],
                            out->nodes[static_cast<std::size_t>(t.c)]};
        for (int i = 0; i < 3; ++i) {
            const Vec3d u = p[(i + 1) % 3] - p[i];
            const Vec3d v = p[(i + 2) % 3] - p[i];
            const double lengths = u.Length() * v.Length();
            if (!(lengths > 0.0)) {
                report->min_surface_angle = 0.0;
                continue;
            }
            const double cosine = cad::Clamp(u.Dot(v) / lengths, -1.0, 1.0);
            report->min_surface_angle =
                std::min(report->min_surface_angle, std::acos(cosine) * 180.0 / cad::kPi);
        }
    }
    if (quality.min_dihedral < options.min_acceptable_dihedral) {
        report->error = "the mesh's worst dihedral angle is " + std::to_string(quality.min_dihedral) +
                        " degrees, below the " + std::to_string(options.min_acceptable_dihedral) +
                        " asked for; it is returned but should not be solved with";
        // NAME THE STAGE THAT IS ACTUALLY AT FAULT. A tetrahedron cannot
        // be better than the boundary triangle it stands on, so when the
        // surface is just as bad, reporting this as a volume-quality
        // failure sends the reader to the wrong file -- it sent me there
        // for most of an afternoon. The volume mesher's flipping and
        // smoothing cannot repair a degenerate input and should not be
        // blamed for failing to.
        if (report->min_surface_angle < options.min_acceptable_dihedral) {
            report->error += ". The surface mesh it was built on is no better -- its worst "
                             "triangle angle is " +
                             std::to_string(report->min_surface_angle) +
                             " degrees -- so this is a surface meshing problem rather than a "
                             "volume one, and a finer or less abruptly graded sizing field is "
                             "what would fix it";
        }
        report->ok = false;
        return false;
    }
    report->ok = true;
    return true;
}

}  // namespace fem
