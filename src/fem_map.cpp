#include "fem_map.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <map>
#include <string>
#include <vector>

namespace fem {
namespace {

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

}  // namespace

bool InsideReference(ElementShape shape, const cad::Vec3d &reference, double tolerance) {
    const double r = reference.x;
    const double s = reference.y;
    const double t = reference.z;
    switch (shape) {
        case ElementShape::Tri3:
        case ElementShape::Tri6:
            return r >= -tolerance && s >= -tolerance && r + s <= 1.0 + tolerance;
        case ElementShape::Quad4:
        case ElementShape::Quad8:
            return std::fabs(r) <= 1.0 + tolerance && std::fabs(s) <= 1.0 + tolerance;
        case ElementShape::Tet4:
        case ElementShape::Tet10:
            return r >= -tolerance && s >= -tolerance && t >= -tolerance &&
                   r + s + t <= 1.0 + tolerance;
        case ElementShape::Hex8:
        case ElementShape::Hex20:
            return std::fabs(r) <= 1.0 + tolerance && std::fabs(s) <= 1.0 + tolerance &&
                   std::fabs(t) <= 1.0 + tolerance;
        case ElementShape::Wedge6:
        case ElementShape::Wedge15:
            return r >= -tolerance && s >= -tolerance && r + s <= 1.0 + tolerance &&
                   std::fabs(t) <= 1.0 + tolerance;
        case ElementShape::Pyr5:
            // The base shrinks to the apex, so the bound on the base
            // coordinates depends on the height.
            return t >= -tolerance && t <= 1.0 + tolerance &&
                   std::fabs(r) <= 1.0 - t + tolerance && std::fabs(s) <= 1.0 - t + tolerance;
    }
    return false;
}

bool InverseMap(ElementShape shape, const std::vector<cad::Vec3d> &nodes, const cad::Vec3d &point,
                cad::Vec3d *out_reference, double *out_distance) {
    const int count = ElementNodeCount(shape);
    if (static_cast<int>(nodes.size()) != count) return false;
    // Started at the reference element's own centre, which is inside it
    // whatever the shape -- including the pyramid, whose centre is not the
    // average of its corners.
    std::vector<cad::Vec3d> reference;
    ReferenceNodes(shape, &reference);
    cad::Vec3d at{};
    for (const cad::Vec3d &p : reference) at = at + p;
    at = at * (1.0 / static_cast<double>(reference.size()));

    std::vector<double> shape_values;
    std::vector<double> dn;
    double best = std::numeric_limits<double>::infinity();
    cad::Vec3d best_at = at;
    for (int step = 0; step < 40; ++step) {
        ShapeFunctions(shape, at, &shape_values, &dn);
        cad::Vec3d mapped{};
        for (int a = 0; a < count; ++a) mapped = mapped + nodes[Idx(a)] * shape_values[Idx(a)];
        const cad::Vec3d error = point - mapped;
        const double distance = error.Length();
        if (distance < best) {
            best = distance;
            best_at = at;
        }
        // The Jacobian of the map, which is the transpose of the one
        // ElementJacobian builds -- there the reference index comes first.
        double jacobian[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
        for (int a = 0; a < count; ++a) {
            const double position[3] = {nodes[Idx(a)].x, nodes[Idx(a)].y, nodes[Idx(a)].z};
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) {
                    jacobian[i][j] += position[i] * dn[Idx(a * 3 + j)];
                }
            }
        }
        const double determinant =
            jacobian[0][0] * (jacobian[1][1] * jacobian[2][2] - jacobian[1][2] * jacobian[2][1]) -
            jacobian[0][1] * (jacobian[1][0] * jacobian[2][2] - jacobian[1][2] * jacobian[2][0]) +
            jacobian[0][2] * (jacobian[1][0] * jacobian[2][1] - jacobian[1][1] * jacobian[2][0]);
        if (!(std::fabs(determinant) > 0.0)) break;
        const double rhs[3] = {error.x, error.y, error.z};
        double correction[3] = {0.0, 0.0, 0.0};
        // Cramer, which for three unknowns is shorter than an elimination
        // and has no pivoting to get wrong.
        for (int column = 0; column < 3; ++column) {
            double m[3][3];
            for (int i = 0; i < 3; ++i) {
                for (int j = 0; j < 3; ++j) m[i][j] = jacobian[i][j];
            }
            for (int i = 0; i < 3; ++i) m[i][column] = rhs[i];
            correction[column] =
                (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) -
                 m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0]) +
                 m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0])) /
                determinant;
        }
        at = at + cad::Vec3d{correction[0], correction[1], correction[2]};
        const double size = std::fabs(correction[0]) + std::fabs(correction[1]) +
                           std::fabs(correction[2]);
        if (size < 1e-13) break;
    }
    *out_reference = best_at;
    *out_distance = best;
    // Converged means the point really is where the map says it is, in
    // units of the element's own size rather than absolute ones.
    double span = 0.0;
    for (const cad::Vec3d &a : nodes) {
        for (const cad::Vec3d &b : nodes) span = std::max(span, (a - b).Length());
    }
    return best <= std::max(span, 1e-300) * 1e-9;
}

bool MapField(const std::vector<cad::Vec3d> &from_nodes,
              const std::vector<BoundElement> &from_elements, const std::vector<double> &values,
              const std::vector<cad::Vec3d> &to, std::vector<double> *out, MapReport *report) {
    *report = MapReport{};
    out->assign(to.size(), 0.0);
    if (from_nodes.size() != values.size()) {
        report->error = "the field does not have one value per node of the mesh it came from";
        return false;
    }
    if (from_elements.empty()) {
        report->error = "the mesh the field came from has no elements";
        return false;
    }

    // A uniform grid over the source elements' bounding boxes, so that a
    // target node only tries the elements near it. Brute force is
    // quadratic and this is the same problem the mesher's crowding test
    // had.
    cad::Box3d extent;
    for (const cad::Vec3d &p : from_nodes) extent.Expand(p);
    for (const cad::Vec3d &p : to) extent.Expand(p);
    const int buckets = std::max(1, std::min(64, static_cast<int>(
                                                    std::cbrt(static_cast<double>(
                                                        from_elements.size())) +
                                                    1.0)));
    const double step_x = std::max(extent.x.Width() / buckets, 1e-300);
    const double step_y = std::max(extent.y.Width() / buckets, 1e-300);
    const double step_z = std::max(extent.z.Width() / buckets, 1e-300);
    auto cell_of = [&](const cad::Vec3d &p) {
        const int i = std::max(0, std::min(buckets - 1,
                                          static_cast<int>((p.x - extent.x.lo) / step_x)));
        const int j = std::max(0, std::min(buckets - 1,
                                          static_cast<int>((p.y - extent.y.lo) / step_y)));
        const int k = std::max(0, std::min(buckets - 1,
                                          static_cast<int>((p.z - extent.z.lo) / step_z)));
        return (k * buckets + j) * buckets + i;
    };
    std::map<int, std::vector<int>> grid;
    std::vector<cad::Vec3d> corner;
    for (std::size_t e = 0; e < from_elements.size(); ++e) {
        cad::Box3d box;
        for (const int node : from_elements[e].nodes) {
            if (node < 0 || node >= static_cast<int>(from_nodes.size())) {
                report->error = "an element of the source mesh names a node that does not exist";
                return false;
            }
            box.Expand(from_nodes[Idx(node)]);
        }
        const int lo = cell_of(cad::Vec3d{box.x.lo, box.y.lo, box.z.lo});
        const int hi = cell_of(cad::Vec3d{box.x.hi, box.y.hi, box.z.hi});
        const int lo_i = lo % buckets;
        const int lo_j = (lo / buckets) % buckets;
        const int lo_k = lo / (buckets * buckets);
        const int hi_i = hi % buckets;
        const int hi_j = (hi / buckets) % buckets;
        const int hi_k = hi / (buckets * buckets);
        for (int k = lo_k; k <= hi_k; ++k) {
            for (int j = lo_j; j <= hi_j; ++j) {
                for (int i = lo_i; i <= hi_i; ++i) {
                    grid[(k * buckets + j) * buckets + i].push_back(static_cast<int>(e));
                }
            }
        }
    }

    std::vector<double> shape;
    std::vector<double> dn;
    for (std::size_t target = 0; target < to.size(); ++target) {
        const cad::Vec3d &point = to[target];
        int chosen = -1;
        cad::Vec3d reference{};
        double nearest = std::numeric_limits<double>::infinity();
        cad::Vec3d nearest_reference{};
        int nearest_element = -1;

        // The cell the point is in first, then its neighbours, then
        // everything -- which only happens for a point well outside.
        std::vector<int> candidates;
        const int cell = cell_of(point);
        const int ci = cell % buckets;
        const int cj = (cell / buckets) % buckets;
        const int ck = cell / (buckets * buckets);
        for (int dk = -1; dk <= 1 && chosen < 0; ++dk) {
            for (int dj = -1; dj <= 1; ++dj) {
                for (int di = -1; di <= 1; ++di) {
                    const int i = ci + di;
                    const int j = cj + dj;
                    const int k = ck + dk;
                    if (i < 0 || j < 0 || k < 0 || i >= buckets || j >= buckets || k >= buckets) {
                        continue;
                    }
                    const auto found = grid.find((k * buckets + j) * buckets + i);
                    if (found == grid.end()) continue;
                    for (const int e : found->second) candidates.push_back(e);
                }
            }
        }
        std::sort(candidates.begin(), candidates.end());
        candidates.erase(std::unique(candidates.begin(), candidates.end()), candidates.end());
        for (int attempt = 0; attempt < 2 && chosen < 0; ++attempt) {
            if (attempt == 1) {
                candidates.clear();
                for (std::size_t e = 0; e < from_elements.size(); ++e) {
                    candidates.push_back(static_cast<int>(e));
                }
            }
            for (const int e : candidates) {
                const BoundElement &element = from_elements[Idx(e)];
                corner.clear();
                for (const int node : element.nodes) corner.push_back(from_nodes[Idx(node)]);
                cad::Vec3d local{};
                double distance = 0.0;
                InverseMap(element.shape, corner, point, &local, &distance);
                if (distance < nearest) {
                    nearest = distance;
                    nearest_reference = local;
                    nearest_element = e;
                }
                // A tolerance in *reference* units, which are the same size
                // whatever the element's physical size -- so this means the
                // same thing on a millimetre part and a metre one, which an
                // absolute distance would not.
                if (!InsideReference(element.shape, local, 1e-8)) continue;
                chosen = e;
                reference = local;
                break;
            }
        }
        if (chosen < 0) {
            if (nearest_element < 0) {
                report->error = "a target point could not be placed in the source mesh at all";
                return false;
            }
            ++report->extrapolated;
            chosen = nearest_element;
            reference = nearest_reference;
            // Clamped back onto the element, so a node a hair outside gets
            // the boundary value rather than an extrapolated one -- which
            // for a field that is about to be differentiated matters more
            // than the hair does.
            const ElementShape shape_of = from_elements[Idx(chosen)].shape;
            for (int guard = 0; guard < 40; ++guard) {
                if (InsideReference(shape_of, reference, 0.0)) break;
                reference = reference * 0.9;
            }
            // HOW FAR OUTSIDE, MEASURED IN METRES, and this used to be
            // the Newton residual instead. The two look alike -- both
            // come back from `InverseMap` and both are small when all is
            // well -- and they are not the same quantity at all: the
            // residual says whether the inverse map *converged*, and an
            // isoparametric map extrapolates perfectly happily, so a
            // point two whole units clear of the model converges to a
            // reference coordinate outside the element with a residual of
            // zero. The field reported no distance for it, which is the
            // one case the field exists for. The distance is from the
            // point to where it landed after clamping, which is the
            // closest point of the element it actually took its value
            // from.
            corner.clear();
            for (const int node : from_elements[Idx(chosen)].nodes) {
                corner.push_back(from_nodes[Idx(node)]);
            }
            std::vector<double> shape_values;
            std::vector<double> shape_gradients;
            ShapeFunctions(shape_of, reference, &shape_values, &shape_gradients);
            cad::Vec3d landed{};
            for (std::size_t a = 0; a < corner.size(); ++a) landed = landed + corner[a] * shape_values[a];
            report->worst_distance = std::max(report->worst_distance, (point - landed).Length());
        } else {
            ++report->inside;
        }
        const BoundElement &element = from_elements[Idx(chosen)];
        ShapeFunctions(element.shape, reference, &shape, &dn);
        double value = 0.0;
        for (std::size_t a = 0; a < element.nodes.size(); ++a) {
            value += shape[a] * values[Idx(element.nodes[a])];
        }
        (*out)[target] = value;
    }
    report->ok = true;
    return true;
}

}  // namespace fem
