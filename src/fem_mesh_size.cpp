// The sizing field (plans/CAD_FEM_PLAN.md Part G.1).
//
// A background octree carrying the element size to aim for at each point.
// Three things seed it, and the reason there are three is that each
// catches something the others cannot:
//
//   CURVATURE, so that a fillet gets elements across it. A face of radius
//   r turning `angle` radians per element wants a size of about r*angle
//   there, whatever the global target says -- and the global target knows
//   nothing about the fillet.
//
//   THIN WALLS, so that a plate two millimetres thick does not get one
//   element through its thickness. Curvature says nothing about this: two
//   flat faces a hair apart are both perfectly flat.
//
//   LOCAL OVERRIDES, so that a user who wants this face finer can say so.
//
// And then a gradation pass, because a field seeded from a small feature
// and left alone jumps straight back to the target one cell away, which
// puts a badly graded element right where the interesting geometry is.

#include "cad_tessellate.h"
#include "fem_mesh.h"

#include <algorithm>
#include <cmath>
#include <limits>

namespace fem {
namespace {

using cad::Vec3d;

// Where a point sits in a cell's eight children.
int ChildIndex(const cad::Box3d &box, const Vec3d &p) {
    const Vec3d centre = box.Center();
    return (p.x >= centre.x ? 1 : 0) | (p.y >= centre.y ? 2 : 0) | (p.z >= centre.z ? 4 : 0);
}

// How far a point is from a box: zero inside it, the straight-line
// distance to its nearest face or corner outside. Using the distance to
// the *centre* instead would make even a uniform field read back as
// something other than itself, since no point but the centre is at zero.
double DistanceToBox(const cad::Box3d &box, const Vec3d &p) {
    const double dx = std::max({box.x.lo - p.x, 0.0, p.x - box.x.hi});
    const double dy = std::max({box.y.lo - p.y, 0.0, p.y - box.y.hi});
    const double dz = std::max({box.z.lo - p.z, 0.0, p.z - box.z.hi});
    return std::sqrt(dx * dx + dy * dy + dz * dz);
}

cad::Box3d ChildBox(const cad::Box3d &box, int index) {
    const Vec3d centre = box.Center();
    cad::Box3d out;
    out.x = (index & 1) != 0 ? cad::Interval{centre.x, box.x.hi} : cad::Interval{box.x.lo, centre.x};
    out.y = (index & 2) != 0 ? cad::Interval{centre.y, box.y.hi} : cad::Interval{box.y.lo, centre.y};
    out.z = (index & 4) != 0 ? cad::Interval{centre.z, box.z.hi} : cad::Interval{box.z.lo, centre.z};
    return out;
}

}  // namespace

int SizingField::LeafAt(const Vec3d &point) const {
    if (cells_.empty()) return -1;
    int at = 0;
    for (;;) {
        const Cell &cell = cells_[static_cast<std::size_t>(at)];
        if (cell.IsLeaf()) return at;
        at = cell.children[ChildIndex(cell.box, point)];
    }
}

// Drives a size into the tree, subdividing until the leaf is no bigger
// than the size being asked for. That is the rule that makes the octree
// carry a *field* rather than a scatter of samples: a cell may only hold
// a size it is small enough to represent.
void SizingField::LeavesOverlapping(int cell, const cad::Box3d &region, std::vector<int> *out) const {
    if (cell < 0 || cell >= static_cast<int>(cells_.size())) return;
    const Cell &here = cells_[static_cast<std::size_t>(cell)];
    if (here.box.x.hi < region.x.lo || here.box.x.lo > region.x.hi) return;
    if (here.box.y.hi < region.y.lo || here.box.y.lo > region.y.hi) return;
    if (here.box.z.hi < region.z.lo || here.box.z.lo > region.z.hi) return;
    if (here.IsLeaf()) {
        out->push_back(cell);
        return;
    }
    for (int child : here.children) LeavesOverlapping(child, region, out);
}

int SizingField::Insert(const Vec3d &point, double size, int depth_limit) {
    if (cells_.empty()) return -1;
    size = std::max(size, min_size_);
    int at = 0;
    for (int depth = 0; depth < depth_limit; ++depth) {
        const cad::Box3d &box = cells_[static_cast<std::size_t>(at)].box;
        const double width = std::max({box.x.Width(), box.y.Width(), box.z.Width()});
        if (width <= size || depth + 1 >= depth_limit) break;
        if (cells_[static_cast<std::size_t>(at)].IsLeaf()) {
            // THE CHILDREN INHERIT WHAT THE CELL HAD, not what is being
            // driven in. Lowering the cell on the way down and then
            // handing that to all eight children spreads one local
            // refinement across the whole branch, which showed up as a
            // 0.1 refinement in the middle of a box making the entire box
            // 0.1. Only the leaf the point lands in takes the new size;
            // Smooth() below then carries the minimum back up and grades
            // it outwards, which is where a size is supposed to spread.
            const cad::Box3d parent = cells_[static_cast<std::size_t>(at)].box;
            const double inherited = cells_[static_cast<std::size_t>(at)].size;
            for (int i = 0; i < 8; ++i) {
                Cell child;
                child.box = ChildBox(parent, i);
                child.size = inherited;
                cells_.push_back(child);
                cells_[static_cast<std::size_t>(at)].children[i] = static_cast<int>(cells_.size()) - 1;
            }
        }
        at = cells_[static_cast<std::size_t>(at)]
                 .children[ChildIndex(cells_[static_cast<std::size_t>(at)].box, point)];
    }
    cells_[static_cast<std::size_t>(at)].size =
        std::min(cells_[static_cast<std::size_t>(at)].size, size);
    return at;
}

void SizingField::Refine(const Vec3d &centre, double radius, double size) {
    if (cells_.empty() || !(radius > 0.0)) return;
    // Driven in at a scatter of points across the ball rather than only
    // at its centre, because a single insertion refines one chain of
    // cells and leaves the rest of the ball at whatever it was.
    const int steps = 3;
    for (int i = -steps; i <= steps; ++i) {
        for (int j = -steps; j <= steps; ++j) {
            for (int k = -steps; k <= steps; ++k) {
                const Vec3d offset{static_cast<double>(i), static_cast<double>(j),
                                   static_cast<double>(k)};
                if (offset.Length() > static_cast<double>(steps)) continue;
                Insert(centre + offset * (radius / static_cast<double>(steps)), size, max_depth_);
            }
        }
    }
}

// The gradation pass.
//
// THE RULE IS PER UNIT OF DISTANCE, NOT PER CELL. "Neighbouring cells may
// differ by at most `growth`" sounds right and is not: how fast the size
// then grows depends on how finely the octree happens to be divided
// there, so a seed far from anything else grows four times across a
// coarse region and forty times across a fine one. The condition a
// mesher actually wants is Lipschitz -- the size may increase by at most
// (growth - 1) per unit travelled -- which is a statement about the
// model and not about the data structure. Relaxed outwards from the
// small cells until nothing changes.
void SizingField::Smooth() {
    if (cells_.empty()) return;
    if (!(growth_ > 1.0)) return;
    // Leaves only: an interior cell's size is the minimum of its
    // children's and is recomputed at the end.
    std::vector<int> leaves;
    for (std::size_t i = 0; i < cells_.size(); ++i) {
        if (cells_[i].IsLeaf()) leaves.push_back(static_cast<int>(i));
    }
    // Relaxation by repeated sweeps. The number of sweeps needed is the
    // number of cells a small size has to travel, which is bounded by the
    // tree's width; twenty is comfortably more than eight levels needs.
    std::vector<int> neighbours;
    for (int pass = 0; pass < 40; ++pass) {
        bool changed = false;
        for (std::size_t i = 0; i < leaves.size(); ++i) {
            Cell &cell = cells_[static_cast<std::size_t>(leaves[i])];
            const cad::Box3d box = cell.box;
            const double nudge = min_size_ * 0.25;
            neighbours.clear();
            for (int axis = 0; axis < 3; ++axis) {
                for (int side = -1; side <= 1; side += 2) {
                    // A thin slab just outside one face, and every leaf
                    // that overlaps it. Exact, where probing the face at
                    // a few points is not: a coarse cell next to a dozen
                    // fine ones has a dozen neighbours, and the ones a
                    // probe misses are the ones that never get graded.
                    cad::Box3d slab = box;
                    cad::Interval *along = axis == 0 ? &slab.x : (axis == 1 ? &slab.y : &slab.z);
                    if (side < 0) {
                        along->hi = along->lo - nudge * 0.5;
                        along->lo = along->hi - nudge * 0.5;
                    } else {
                        along->lo = along->hi + nudge * 0.5;
                        along->hi = along->lo + nudge * 0.5;
                    }
                    LeavesOverlapping(0, slab, &neighbours);
                }
            }
            for (int neighbour : neighbours) {
                const Cell &other = cells_[static_cast<std::size_t>(neighbour)];
                const double distance = std::max(nudge, DistanceToBox(other.box, box.Center()));
                const double allowed = other.size + (growth_ - 1.0) * distance;
                if (cell.size > allowed) {
                    cell.size = allowed;
                    changed = true;
                }
            }
        }
        if (!changed) break;
    }
    // Interior cells carry the smallest of their children, so a coarse
    // lookup never over-estimates.
    for (std::size_t i = cells_.size(); i-- > 0;) {
        if (cells_[i].IsLeaf()) continue;
        double smallest = cells_[i].size;
        for (int child : cells_[i].children) {
            smallest = std::min(smallest, cells_[static_cast<std::size_t>(child)].size);
        }
        cells_[i].size = smallest;
    }
}

double SizingField::At(const Vec3d &point) const {
    if (cells_.empty()) return target_;
    // Outside the tree, the nearest cell's size. The volume mesher asks
    // about circumcentres that wander out, and answering "no idea" would
    // make it refuse points it should merely have sized coarsely.
    Vec3d clamped = point;
    clamped.x = std::max(extent_.x.lo, std::min(extent_.x.hi, clamped.x));
    clamped.y = std::max(extent_.y.lo, std::min(extent_.y.hi, clamped.y));
    clamped.z = std::max(extent_.z.lo, std::min(extent_.z.hi, clamped.z));
    const int leaf = LeafAt(clamped);
    if (leaf < 0) return target_;

    // THE LIPSCHITZ ENVELOPE OF THE NEIGHBOURHOOD, not the cell's own
    // constant. An octree stores one size per cell, so reading it back
    // raw gives a field that jumps at cell boundaries -- which are
    // invisible, have nothing to do with the geometry, and would put a
    // step change in element size wherever one happened to fall. Taking
    // the smallest of (a nearby cell's size plus the gradient times the
    // distance to it) gives back exactly the continuous field the
    // gradation pass promised.
    const cad::Box3d box = cells_[static_cast<std::size_t>(leaf)].box;
    const double gradient = growth_ - 1.0;
    double best = cells_[static_cast<std::size_t>(leaf)].size + gradient * DistanceToBox(box, clamped);
    const double nudge = min_size_ * 0.25;
    std::vector<int> neighbours;
    for (int axis = 0; axis < 3; ++axis) {
        for (int side = -1; side <= 1; side += 2) {
            cad::Box3d slab = box;
            cad::Interval *along = axis == 0 ? &slab.x : (axis == 1 ? &slab.y : &slab.z);
            if (side < 0) {
                along->hi = along->lo - nudge * 0.5;
                along->lo = along->hi - nudge * 0.5;
            } else {
                along->lo = along->hi + nudge * 0.5;
                along->hi = along->lo + nudge * 0.5;
            }
            LeavesOverlapping(0, slab, &neighbours);
        }
    }
    for (int neighbour : neighbours) {
        const cad::Box3d &other = cells_[static_cast<std::size_t>(neighbour)].box;
        best = std::min(best, cells_[static_cast<std::size_t>(neighbour)].size +
                                  gradient * DistanceToBox(other, clamped));
    }
    return std::max(min_size_, std::min(max_size_, best));
}

bool SizingField::Build(const cad::Model &model, const std::vector<cad::EntityId> &bodies,
                        const SizingOptions &options, std::string *error) {
    error->clear();
    cells_.clear();
    if (bodies.empty()) {
        *error = "there are no bodies to size";
        return false;
    }

    cad::Box3d extent;
    for (cad::EntityId body : bodies) {
        const cad::Body *solid = model.GetBody(body);
        if (solid == nullptr) continue;
        for (cad::EntityId shell : solid->shells) {
            for (cad::EntityId vertex : model.VerticesOfShell(shell)) {
                extent.Expand(model.GetVertex(vertex)->point);
            }
            for (cad::EntityId face : model.GetShell(shell)->faces) {
                const cad::Box3d face_box = model.BoundsOfFace(face);
                if (face_box.IsEmpty()) continue;
                extent.Expand(cad::Vec3d{face_box.x.lo, face_box.y.lo, face_box.z.lo});
                extent.Expand(cad::Vec3d{face_box.x.hi, face_box.y.hi, face_box.z.hi});
            }
        }
    }
    if (extent.IsEmpty()) {
        *error = "the bodies have no extent";
        return false;
    }
    const double diagonal =
        cad::Vec3d{extent.x.Width(), extent.y.Width(), extent.z.Width()}.Length();
    target_ = options.target > 0.0 ? options.target : diagonal / 20.0;
    min_size_ = options.min_size > 0.0 ? options.min_size : target_ / 100.0;
    max_size_ = options.max_size > 0.0 ? options.max_size : target_ * 2.0;
    growth_ = std::max(1.01, options.growth);
    max_depth_ = std::max(1, options.max_depth);

    // The root is a cube: an octree over a long thin box would otherwise
    // have cells as long as the box however deep it went.
    const cad::Vec3d centre = extent.Center();
    const double half = std::max(diagonal, 1e-9) * 0.55;
    extent_ = cad::Box3d{};
    extent_.Expand(centre - cad::Vec3d{half, half, half});
    extent_.Expand(centre + cad::Vec3d{half, half, half});
    Cell root;
    root.box = extent_;
    root.size = target_;
    cells_.push_back(root);

    // --- Curvature ---------------------------------------------------------
    //
    // Sampled over each face's parameter domain rather than at its
    // corners: a fillet's curvature is the same everywhere on it, but a
    // blended or NURBS face's is not, and the corners are exactly where a
    // face is least curved.
    // SAMPLED TWICE, and the second time at a density set by what the
    // first found. A fixed grid cannot work: seven samples along a
    // cylinder twenty units long are three units apart, while the size
    // they are asking for is a third of a unit, so the field ends up with
    // forty-nine fine cells and coarse everything between them. The first
    // pass finds the smallest size the face wants; the second lays it
    // down closely enough to cover.
    for (cad::EntityId body : bodies) {
        const cad::Body *solid = model.GetBody(body);
        if (solid == nullptr) continue;
        for (cad::EntityId shell : solid->shells) {
            for (cad::EntityId face_id : model.GetShell(shell)->faces) {
                const cad::Face *face = model.GetFace(face_id);
                const cad::Surface *surface = model.SurfaceAt(face->surface);
                if (surface == nullptr) continue;
                double u_lo = 0.0;
                double u_hi = 0.0;
                double v_lo = 0.0;
                double v_hi = 0.0;
                surface->Domain(&u_lo, &u_hi, &v_lo, &v_hi);
                auto size_at = [&](double u, double v) {
                    double k1 = 0.0;
                    double k2 = 0.0;
                    surface->PrincipalCurvatures(u, v, &k1, &k2);
                    const double curvature = std::max(std::fabs(k1), std::fabs(k2));
                    if (!(curvature > 0.0) || !std::isfinite(curvature)) return target_;
                    return std::max(min_size_,
                                    std::min(target_, options.curvature_angle / curvature));
                };
                double smallest = target_;
                for (int i = 0; i <= 6; ++i) {
                    for (int j = 0; j <= 6; ++j) {
                        smallest = std::min(
                            smallest,
                            size_at(u_lo + (u_hi - u_lo) * static_cast<double>(i) / 6.0,
                                    v_lo + (v_hi - v_lo) * static_cast<double>(j) / 6.0));
                    }
                }
                // A flat face asks for nothing, so there is nothing to
                // lay down and no reason to walk it.
                if (!(smallest < target_ * 0.999)) continue;
                // How long the domain is in space, so the sample count
                // can be chosen in units the size is measured in.
                //
                // WALKED, NOT MEASURED END TO END. A closed surface's two
                // ends are the same point, so subtracting them gives zero
                // and the sampling collapses to three positions around a
                // cylinder -- which then seeds the field at three angles
                // and leaves the rest of the circumference at the target.
                auto walk = [&](bool along_u) {
                    double total = 0.0;
                    const int steps = 8;
                    cad::Vec3d previous;
                    for (int k = 0; k <= steps; ++k) {
                        const double s = static_cast<double>(k) / static_cast<double>(steps);
                        const cad::Vec3d here =
                            along_u ? surface->Point(u_lo + (u_hi - u_lo) * s, 0.5 * (v_lo + v_hi))
                                    : surface->Point(0.5 * (u_lo + u_hi), v_lo + (v_hi - v_lo) * s);
                        if (k > 0) total += (here - previous).Length();
                        previous = here;
                    }
                    return total;
                };
                const double span_u = walk(true);
                const double span_v = walk(false);
                const int cap = 96;
                const int steps_u = std::max(
                    2, std::min(cap, static_cast<int>(std::ceil(span_u / (smallest * 0.7)))));
                const int steps_v = std::max(
                    2, std::min(cap, static_cast<int>(std::ceil(span_v / (smallest * 0.7)))));
                for (int i = 0; i <= steps_u; ++i) {
                    for (int j = 0; j <= steps_v; ++j) {
                        const double u = u_lo + (u_hi - u_lo) * static_cast<double>(i) /
                                                    static_cast<double>(steps_u);
                        const double v = v_lo + (v_hi - v_lo) * static_cast<double>(j) /
                                                    static_cast<double>(steps_v);
                        Insert(surface->Point(u, v), size_at(u, v), max_depth_);
                    }
                }
            }
        }
    }

    // --- Thin walls ---------------------------------------------------------
    //
    // Found on the tessellation rather than on the B-rep: the question is
    // "how far is it through the material from here", which is a ray cast
    // and wants triangles. A face's own triangles are excluded from its
    // own cast, or every face would find itself at zero distance.
    if (options.thin_wall) {
        cad::TessellationMesh mesh;
        std::string tessellation_error;
        bool built = true;
        for (cad::EntityId body : bodies) {
            cad::TessellationMesh one;
            if (!cad::TessellateBody(model, body, {}, &one, &tessellation_error)) {
                built = false;
                break;
            }
            const int base = mesh.VertexCount();
            for (const Vec3d &p : one.positions) mesh.positions.push_back(p);
            for (const Vec3d &n : one.normals) mesh.normals.push_back(n);
            for (int index : one.indices) mesh.indices.push_back(base + index);
            for (cad::EntityId f : one.triangle_face) mesh.triangle_face.push_back(f);
        }
        if (built) {
            const std::size_t triangles = static_cast<std::size_t>(mesh.TriangleCount());
            // SAMPLED ACROSS EACH TRIANGLE, not only at its centroid. A
            // plate forty units square is two triangles per face, so a
            // seed at the centroid alone refines one chain of cells and
            // leaves the middle of the plate at the target -- which is
            // exactly the shape this pass exists to catch. The sampling
            // density is set by the size being looked for rather than by
            // a constant: a triangle much bigger than the target needs
            // many samples and one about the target's size needs one.
            const int budget = triangles > 4000 ? 1 : (triangles > 500 ? 3 : 12);
            for (std::size_t t = 0; t < triangles; ++t) {
                const Vec3d &a = mesh.positions[static_cast<std::size_t>(mesh.indices[t * 3])];
                const Vec3d &b = mesh.positions[static_cast<std::size_t>(mesh.indices[t * 3 + 1])];
                const Vec3d &c = mesh.positions[static_cast<std::size_t>(mesh.indices[t * 3 + 2])];
                const Vec3d normal = (b - a).Cross(c - a);
                if (!(normal.LengthSquared() > 0.0)) continue;
                const Vec3d inward = normal.Normalized() * -1.0;
                const double longest =
                    std::max({(b - a).Length(), (c - b).Length(), (a - c).Length()});
                const int steps = std::max(
                    1, std::min(budget, static_cast<int>(std::ceil(longest / (target_ * 0.5)))));
                for (int i = 0; i <= steps; ++i) {
                    for (int j = 0; i + j <= steps; ++j) {
                        const double wa = static_cast<double>(steps - i - j) / steps;
                        const double wb = static_cast<double>(i) / steps;
                        const double wc = static_cast<double>(j) / steps;
                        const Vec3d from = a * wa + b * wb + c * wc;
                        double nearest = std::numeric_limits<double>::infinity();
                        for (std::size_t other = 0; other < triangles; ++other) {
                            if (other == t) continue;
                            const Vec3d &p0 =
                                mesh.positions[static_cast<std::size_t>(mesh.indices[other * 3])];
                            const Vec3d &p1 =
                                mesh.positions[static_cast<std::size_t>(mesh.indices[other * 3 + 1])];
                            const Vec3d &p2 =
                                mesh.positions[static_cast<std::size_t>(mesh.indices[other * 3 + 2])];
                            const Vec3d edge1 = p1 - p0;
                            const Vec3d edge2 = p2 - p0;
                            const Vec3d h = inward.Cross(edge2);
                            const double determinant = edge1.Dot(h);
                            if (std::fabs(determinant) < 1e-15) continue;
                            const double inverse = 1.0 / determinant;
                            const Vec3d s = from - p0;
                            const double u = inverse * s.Dot(h);
                            if (u < 0.0 || u > 1.0) continue;
                            const Vec3d q = s.Cross(edge1);
                            const double v = inverse * inward.Dot(q);
                            if (v < 0.0 || u + v > 1.0) continue;
                            const double distance = inverse * edge2.Dot(q);
                            if (!(distance > target_ * 1e-6) || distance >= nearest) continue;
                            // THE FAR SIDE OF A WALL FACES BACK AT YOU.
                            // Without that, a sample sitting on a reflex
                            // edge shoots along the plane of the face
                            // that meets it there, and a ray coplanar
                            // with a triangle reports a hit at very
                            // nearly zero distance -- a wall of no
                            // thickness, which drives the size to its
                            // floor and buries the body in elements. On
                            // an L-shaped block three samples out of
                            // thousands did that and took the surface
                            // mesh from about two hundred triangles to
                            // four and a half thousand. A grazed face is
                            // edge-on to the ray, so its normal is
                            // perpendicular to it and this rejects it,
                            // while the real far side of a wall points
                            // back along the ray and passes easily.
                            const Vec3d hit_normal = edge1.Cross(edge2);
                            const double facing = hit_normal.LengthSquared() > 0.0
                                                      ? hit_normal.Normalized().Dot(inward)
                                                      : 0.0;
                            if (facing < 0.1) continue;
                            nearest = distance;
                        }
                        if (!std::isfinite(nearest)) continue;
                        if (nearest >= target_ * static_cast<double>(options.thin_wall_elements)) {
                            // Not thin: the target already puts enough
                            // elements through it.
                            continue;
                        }
                        const double size = std::max(
                            min_size_, nearest / std::max(1, options.thin_wall_elements));
                        // Both faces of the wall and the material between
                        // them, so the interior is sized too rather than
                        // only its skin.
                        Insert(from, size, max_depth_);
                        Insert(from + inward * (nearest * 0.5), size, max_depth_);
                    }
                }
            }
        }
    }

    // The caller's own refinements, before the smoothing, so that the
    // growth limit grades away from them as it does from every other
    // source of size. Applied after them and not before, because a
    // refinement asked for explicitly should win over the geometry's own
    // opinion rather than be averaged with it.
    for (const SizingOptions::Refinement &refinement : options.refinements) {
        if (!(refinement.radius > 0.0) || !(refinement.size > 0.0)) continue;
        Refine(refinement.centre, refinement.radius, refinement.size);
    }

    Smooth();
    return true;
}

}  // namespace fem
