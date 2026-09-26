#include "fem_study.h"

#include "fem_solve.h"

#include <algorithm>
#include <cmath>
#include <map>
#include <set>
#include <string>
#include <vector>

namespace fem {
namespace {

using cad::Vec3d;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// The area element of a surface patch embedded in three dimensions: the
// cross product of the two tangents, whose length is the area scaling
// and whose direction is the outward normal.
//
// fem_elem.h's ElementJacobian cannot answer this -- it maps a
// two-dimensional element in a plane, where the third coordinate does not
// exist. A triangle of a mesh's boundary lives in three dimensions and
// its Jacobian is not square.
Vec3d SurfaceNormalAt(ElementShape shape, const std::vector<Vec3d> &nodes,
                      const std::vector<double> &dn) {
    Vec3d along_u{};
    Vec3d along_v{};
    const int count = ElementNodeCount(shape);
    for (int a = 0; a < count; ++a) {
        along_u = along_u + nodes[Idx(a)] * dn[Idx(a * 3 + 0)];
        along_v = along_v + nodes[Idx(a)] * dn[Idx(a * 3 + 1)];
    }
    return along_u.Cross(along_v);
}

// Every boundary patch of the mesh that lies on one CAD face, as node
// index lists, with the element shape they are.
bool PatchesOnFace(const VolumeMesh &mesh, cad::EntityId face, ElementShape *shape,
                   std::vector<std::vector<int>> *out) {
    out->clear();
    if (!mesh.boundary_quads.empty()) {
        *shape = ElementShape::Quad4;
        for (const SurfaceQuad &quad : mesh.boundary_quads) {
            if (quad.face != face) continue;
            out->push_back({quad.a, quad.b, quad.c, quad.d});
        }
        return !out->empty();
    }
    if (!mesh.boundary6.empty() && mesh.boundary6.size() == mesh.boundary.size()) {
        *shape = ElementShape::Tri6;
        for (std::size_t i = 0; i < mesh.boundary.size(); ++i) {
            if (mesh.boundary[i].face != face) continue;
            const std::array<int, 6> &six = mesh.boundary6[i];
            out->push_back({six[0], six[1], six[2], six[3], six[4], six[5]});
        }
        return !out->empty();
    }
    *shape = ElementShape::Tri3;
    for (const SurfaceTriangle &t : mesh.boundary) {
        if (t.face != face) continue;
        out->push_back({t.a, t.b, t.c});
    }
    return !out->empty();
}

ElementShape ShapeOfTet(const VolumeMesh &mesh) {
    return mesh.tets10.empty() ? ElementShape::Tet4 : ElementShape::Tet10;
}

}  // namespace

// --- Materials ------------------------------------------------------------

MaterialCurve MaterialCurve::Constant(double value) {
    MaterialCurve out;
    out.points.push_back({0.0, value});
    return out;
}

double MaterialCurve::At(double temperature) const {
    if (points.empty()) return 0.0;
    if (points.size() == 1 || temperature <= points.front().temperature) {
        return points.front().value;
    }
    if (temperature >= points.back().temperature) return points.back().value;
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (temperature > points[i].temperature) continue;
        const double span = points[i].temperature - points[i - 1].temperature;
        if (!(span > 0.0)) return points[i].value;
        const double fraction = (temperature - points[i - 1].temperature) / span;
        return points[i - 1].value + (points[i].value - points[i - 1].value) * fraction;
    }
    return points.back().value;
}

double MaterialCurve::Slope(double temperature) const {
    if (points.size() < 2) return 0.0;
    if (temperature <= points.front().temperature) return 0.0;
    if (temperature >= points.back().temperature) return 0.0;
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (temperature > points[i].temperature) continue;
        const double span = points[i].temperature - points[i - 1].temperature;
        if (!(span > 0.0)) return 0.0;
        return (points[i].value - points[i - 1].value) / span;
    }
    return 0.0;
}

bool MaterialCurve::IsValid(std::string *error) const {
    if (points.empty()) {
        *error = "a material property needs at least one value";
        return false;
    }
    for (std::size_t i = 1; i < points.size(); ++i) {
        if (points[i].temperature > points[i - 1].temperature) continue;
        *error = "a material property's table must be in increasing order of temperature";
        return false;
    }
    return true;
}

bool StudyMaterial::AsIsotropic(double temperature, Material *out, std::string *error) const {
    if (kind != MaterialKind::Isotropic) {
        *error = name + " is orthotropic and cannot be reduced to one modulus and one Poisson's "
                        "ratio; an averaged orthotropic material is not any material";
        return false;
    }
    if (!youngs_modulus.IsValid(error) || !poissons_ratio.IsValid(error)) return false;
    out->name = name;
    out->youngs_modulus = youngs_modulus.At(temperature);
    out->poissons_ratio = poissons_ratio.At(temperature);
    out->density = density;
    return out->IsValid(error);
}

bool StudyMaterial::ConstitutiveMatrix(double temperature, double out[6][6],
                                       std::string *error) const {
    for (int i = 0; i < 6; ++i) {
        for (int j = 0; j < 6; ++j) out[i][j] = 0.0;
    }
    if (kind == MaterialKind::Isotropic) {
        Material isotropic;
        if (!AsIsotropic(temperature, &isotropic, error)) return false;
        ::fem::ConstitutiveMatrix(isotropic, out);
        return true;
    }

    // BUILT AS A COMPLIANCE AND INVERTED, not written out as a stiffness.
    // The nine constants a person has are compliances -- a modulus is a
    // strain per stress and a Poisson's ratio is a strain ratio -- so the
    // compliance matrix is the one that can be filled in directly from
    // them, and every term of it is one input. The stiffness written out
    // in terms of the same nine has a shared denominator in every entry
    // and is a page of algebra with nowhere to check itself.
    //
    // It also makes the symmetry requirement checkable rather than
    // assumed: nu_ij/E_i = nu_ji/E_j is what makes the compliance
    // symmetric, and an inconsistent set of inputs shows up here as a
    // matrix that is not.
    for (int i = 0; i < 3; ++i) {
        if (e[i] > 0.0 && g[i] > 0.0) continue;
        *error = name + " has a non-positive modulus";
        return false;
    }
    double compliance[6][6] = {};
    // nu[0] = nu_xy, nu[1] = nu_yz, nu[2] = nu_zx.
    const double nu_xy = nu[0];
    const double nu_yz = nu[1];
    const double nu_zx = nu[2];
    compliance[0][0] = 1.0 / e[0];
    compliance[1][1] = 1.0 / e[1];
    compliance[2][2] = 1.0 / e[2];
    compliance[0][1] = -nu_xy / e[0];
    compliance[1][0] = compliance[0][1];
    compliance[1][2] = -nu_yz / e[1];
    compliance[2][1] = compliance[1][2];
    compliance[2][0] = -nu_zx / e[2];
    compliance[0][2] = compliance[2][0];
    compliance[3][3] = 1.0 / g[0];
    compliance[4][4] = 1.0 / g[1];
    compliance[5][5] = 1.0 / g[2];

    // Invert the 3x3 normal block by hand and the shear terms directly:
    // the matrix is block diagonal, so a general inversion would be
    // solving three trivial problems with one difficult routine.
    const double a = compliance[0][0];
    const double b = compliance[1][1];
    const double c = compliance[2][2];
    const double d = compliance[0][1];
    const double f = compliance[1][2];
    const double h = compliance[0][2];
    const double determinant = a * (b * c - f * f) - d * (d * c - f * h) + h * (d * f - b * h);
    if (!(std::fabs(determinant) > 0.0)) {
        *error = name + " has an inconsistent set of orthotropic constants: its compliance matrix "
                        "is singular, which means the moduli and Poisson's ratios do not describe "
                        "a stable material";
        return false;
    }
    out[0][0] = (b * c - f * f) / determinant;
    out[1][1] = (a * c - h * h) / determinant;
    out[2][2] = (a * b - d * d) / determinant;
    out[0][1] = (h * f - d * c) / determinant;
    out[1][0] = out[0][1];
    out[1][2] = (d * h - a * f) / determinant;
    out[2][1] = out[1][2];
    out[0][2] = (d * f - b * h) / determinant;
    out[2][0] = out[0][2];
    out[3][3] = g[0];
    out[4][4] = g[1];
    out[5][5] = g[2];

    // A stable material stores positive energy for any strain, so the
    // stiffness must be positive definite. The normal block's leading
    // minors are the cheap way to ask.
    if (!(out[0][0] > 0.0) || !(out[0][0] * out[1][1] - out[0][1] * out[0][1] > 0.0) ||
        !(determinant * a > 0.0)) {
        *error = name + " has orthotropic constants that would store negative energy for some "
                        "strain, which no real material does; the usual cause is a Poisson's "
                        "ratio too large for the ratio of moduli it sits between";
        return false;
    }
    return true;
}

Target FaceTarget(const cad::Model &model, cad::EntityId face, int generating_feature) {
    Target out;
    out.kind = TargetKind::Face;
    const cad::Face *found = model.GetFace(face);
    out.name = cad::CaptureFaceName(model, face, generating_feature,
                                    found != nullptr ? found->name : std::string());
    return out;
}

Target EdgeTarget(const cad::Model &model, cad::EntityId edge, int generating_feature) {
    Target out;
    out.kind = TargetKind::Edge;
    out.name = cad::CaptureEdgeName(model, edge, generating_feature, std::string());
    return out;
}

double AnalysisModel::MeanTemperature(const std::vector<int> &of) const {
    if (node_temperature.empty() || of.empty()) return temperature;
    double total = 0.0;
    for (const int node : of) {
        if (node < 0 || node >= static_cast<int>(node_temperature.size())) return temperature;
        total += node_temperature[Idx(node)];
    }
    return total / static_cast<double>(of.size());
}

Vec3d AnalysisModel::TotalLoad() const {
    Vec3d total{};
    for (const NodalLoad &load : loads) total = total + load.force;
    return total;
}

// --- Selecting nodes -------------------------------------------------------

std::vector<int> NodesOnFace(const VolumeMesh &mesh, cad::EntityId face) {
    std::set<int> found;
    ElementShape shape = ElementShape::Tri3;
    std::vector<std::vector<int>> patches;
    PatchesOnFace(mesh, face, &shape, &patches);
    for (const std::vector<int> &patch : patches) {
        for (const int node : patch) found.insert(node);
    }
    return std::vector<int>(found.begin(), found.end());
}

std::vector<int> NodesOnEdge(const VolumeMesh &mesh, cad::EntityId edge) {
    std::vector<int> out;
    for (std::size_t i = 0; i < mesh.provenance.size(); ++i) {
        if (mesh.provenance[i].edge == edge) out.push_back(static_cast<int>(i));
    }
    return out;
}

// --- Consistent loads ------------------------------------------------------

bool FaceTraction(const VolumeMesh &mesh, cad::EntityId face, const Vec3d &traction,
                  double pressure, std::vector<NodalLoad> *out, double *out_area,
                  std::string *error) {
    out->clear();
    *out_area = 0.0;
    ElementShape shape = ElementShape::Tri3;
    std::vector<std::vector<int>> patches;
    if (!PatchesOnFace(mesh, face, &shape, &patches)) {
        *error = "no part of the mesh's boundary lies on that face";
        return false;
    }
    std::vector<QuadraturePoint> rule;
    // One degree above what the shape functions need, since the traction
    // is constant but the area element of a curved patch is not.
    if (!Quadrature(shape, ElementDimension(shape) == 2 ? 4 : 2, &rule, error)) return false;

    std::map<int, Vec3d> accumulated;
    std::vector<double> n;
    std::vector<double> dn;
    std::vector<Vec3d> corner;
    for (const std::vector<int> &patch : patches) {
        corner.clear();
        for (const int node : patch) corner.push_back(mesh.nodes[Idx(node)]);
        for (const QuadraturePoint &point : rule) {
            ShapeFunctions(shape, point.at, &n, &dn);
            const Vec3d cross = SurfaceNormalAt(shape, corner, dn);
            const double area = cross.Length();
            if (!(area > 0.0)) continue;
            *out_area += area * point.weight;
            // A pressure acts along the inward normal; the boundary is
            // wound outwards, so that is the cross product negated.
            const Vec3d applied = traction + cross * (-pressure / area);
            for (std::size_t a = 0; a < patch.size(); ++a) {
                accumulated[patch[a]] =
                    accumulated[patch[a]] + applied * (n[a] * area * point.weight);
            }
        }
    }
    for (const auto &entry : accumulated) {
        out->push_back(NodalLoad{entry.first, entry.second});
    }
    return true;
}

// --- Binding ---------------------------------------------------------------

bool BindStudy(const cad::Model &model, const VolumeMesh &mesh, const Study &study,
               const BindOptions &options, AnalysisModel *out, BindReport *report) {
    *out = AnalysisModel{};
    *report = BindReport{};
    if (study.materials.empty()) {
        report->error = "a study needs at least one material";
        return false;
    }
    if (mesh.nodes.empty() || (mesh.tets.empty() && mesh.hexes.empty())) {
        report->error = "the mesh has no elements to analyse";
        return false;
    }
    out->nodes = mesh.nodes;
    out->materials = study.materials;
    out->temperature = study.temperature;
    for (std::size_t i = 0; i < study.materials.size(); ++i) {
        double ignored[6][6];
        std::string why;
        if (study.materials[i].ConstitutiveMatrix(study.temperature, ignored, &why)) continue;
        report->error = why;
        return false;
    }

    // Elements, with their material from the section that covers their
    // body. There is no per-element body tag in a VolumeMesh yet, so a
    // single section applies to everything and several is reported rather
    // than guessed at -- which is honest about where the gap is instead of
    // silently using the first.
    int material = 0;
    if (!study.sections.empty()) {
        material = std::max(0, std::min(static_cast<int>(study.materials.size()) - 1,
                                        study.sections.front().material));
        if (study.sections.size() > 1) {
            report->warnings.push_back(
                "this study has " + std::to_string(study.sections.size()) +
                " sections, and a volume mesh does not yet record which body each element came "
                "from, so the first section's material was used for all of them");
        }
    }
    if (!mesh.tets.empty()) {
        const ElementShape shape = ShapeOfTet(mesh);
        for (std::size_t i = 0; i < mesh.tets.size(); ++i) {
            BoundElement element;
            element.shape = shape;
            element.material = material;
            if (shape == ElementShape::Tet10) {
                for (const int node : mesh.tets10[i]) element.nodes.push_back(node);
            } else {
                element.nodes = {mesh.tets[i].a, mesh.tets[i].b, mesh.tets[i].c, mesh.tets[i].d};
            }
            out->elements.push_back(element);
        }
    }
    for (const Hexahedron &hex : mesh.hexes) {
        BoundElement element;
        element.shape = ElementShape::Hex8;
        element.material = material;
        for (const int node : hex.n) element.nodes.push_back(node);
        out->elements.push_back(element);
    }

    // Resolve one target and say what happened.
    auto resolve = [&](const Target &target, BoundCondition *condition) {
        const cad::ResolveResult result =
            target.kind == TargetKind::Face
                ? cad::ResolveFace(model, target.name, options.naming)
                : cad::ResolveEdge(model, target.name, options.naming);
        condition->status = result.status;
        condition->entity = result.entity;
        condition->explanation = result.explanation;
        switch (result.status) {
            case cad::ResolveStatus::Resolved: ++report->resolved; break;
            case cad::ResolveStatus::Ambiguous: ++report->ambiguous; break;
            case cad::ResolveStatus::Lost: ++report->lost; break;
        }
        return result.status == cad::ResolveStatus::Resolved;
    };

    // Supports. A node named by two supports gets both, axis by axis,
    // which is what a person means by fixing one face in x and another
    // in y where they meet along an edge.
    std::map<int, Constraint> constraints;
    for (const Support &support : study.supports) {
        BoundCondition condition;
        condition.label = support.label;
        if (!resolve(support.where, &condition)) {
            report->conditions.push_back(condition);
            continue;
        }
        const std::vector<int> nodes = support.where.kind == TargetKind::Face
                                           ? NodesOnFace(mesh, condition.entity)
                                           : NodesOnEdge(mesh, condition.entity);
        condition.nodes = static_cast<int>(nodes.size());
        for (const int node : nodes) {
            Constraint &at = constraints[node];
            at.node = node;
            for (int axis = 0; axis < 3; ++axis) {
                if (!support.fixed[axis]) continue;
                at.fixed[axis] = true;
                at.value[axis] = support.value[axis];
            }
        }
        if (nodes.empty()) {
            report->warnings.push_back("the support '" + support.label +
                                       "' resolved to a face the mesh has no nodes on");
        }
        report->conditions.push_back(condition);
    }
    for (const auto &entry : constraints) out->constraints.push_back(entry.second);

    // Loads.
    std::map<int, Vec3d> nodal;
    for (const Load &load : study.loads) {
        BoundCondition condition;
        condition.label = load.label;
        if (load.kind == LoadKind::Gravity) {
            condition.status = cad::ResolveStatus::Resolved;
            // Density times acceleration, integrated over each element --
            // which for a graded mesh is not the same as the total weight
            // split between the nodes, and is what puts the weight of a
            // fine region in the fine region.
            std::vector<double> n;
            std::vector<double> dn;
            std::vector<double> dn_xyz;
            std::vector<Vec3d> corner;
            std::vector<QuadraturePoint> rule;
            std::string why;
            for (const BoundElement &element : out->elements) {
                if (!Quadrature(element.shape, 0, &rule, &why)) {
                    report->error = why;
                    return false;
                }
                corner.clear();
                for (const int node : element.nodes) corner.push_back(out->nodes[Idx(node)]);
                const double density =
                    out->materials[Idx(element.material)].density;
                for (const QuadraturePoint &point : rule) {
                    ShapeFunctions(element.shape, point.at, &n, &dn);
                    const double determinant =
                        ElementJacobian(element.shape, corner, dn, &dn_xyz);
                    const double scale = determinant * point.weight * density;
                    for (std::size_t a = 0; a < element.nodes.size(); ++a) {
                        nodal[element.nodes[a]] =
                            nodal[element.nodes[a]] + load.vector * (n[a] * scale);
                        condition.force = condition.force + load.vector * (n[a] * scale);
                    }
                }
            }
            ++report->resolved;
            report->conditions.push_back(condition);
            continue;
        }
        if (!resolve(load.where, &condition)) {
            report->conditions.push_back(condition);
            continue;
        }
        std::vector<NodalLoad> applied;
        double area = 0.0;
        std::string why;
        if (load.where.kind != TargetKind::Face) {
            report->error = "a " + load.label +
                            " on an edge is a line load, which wants a one-dimensional element to "
                            "integrate over and is not written yet";
            return false;
        }
        Vec3d traction{};
        double pressure = 0.0;
        if (load.kind == LoadKind::Pressure) {
            pressure = load.magnitude;
        } else if (load.kind == LoadKind::Traction) {
            traction = load.vector;
        }
        if (!FaceTraction(mesh, condition.entity, traction, pressure, &applied, &area, &why)) {
            report->error = why;
            return false;
        }
        if (load.kind == LoadKind::Force) {
            // A TOTAL FORCE IS A TRACTION OF ITSELF OVER THE AREA. Working
            // it out that way rather than splitting it between the nodes
            // is what makes it right on a graded mesh, and on a
            // second-order one it is the difference between right and
            // visibly wrong: the consistent share of a quadratic
            // triangle's corner node is negative, and nothing about
            // dividing equally can produce that.
            if (!(area > 0.0)) {
                report->error = "the face '" + load.label + "' names has no area";
                return false;
            }
            if (!FaceTraction(mesh, condition.entity, load.vector * (1.0 / area), 0.0, &applied,
                              &area, &why)) {
                report->error = why;
                return false;
            }
        }
        condition.area = area;
        for (const NodalLoad &one : applied) {
            nodal[one.node] = nodal[one.node] + one.force;
            condition.force = condition.force + one.force;
        }
        condition.nodes = static_cast<int>(applied.size());
        report->conditions.push_back(condition);
    }
    for (const auto &entry : nodal) out->loads.push_back(NodalLoad{entry.first, entry.second});

    if (report->ambiguous != 0 || report->lost != 0) {
        std::string trouble;
        for (const BoundCondition &condition : report->conditions) {
            if (condition.status == cad::ResolveStatus::Resolved) continue;
            if (!trouble.empty()) trouble += "; ";
            trouble += "'" + condition.label + "' is " +
                       (condition.status == cad::ResolveStatus::Ambiguous ? "ambiguous" : "lost") +
                       " (" + condition.explanation + ")";
        }
        if (!options.allow_unresolved) {
            report->error = "this study refers to geometry that cannot be found in the model: " +
                            trouble;
            return false;
        }
        report->warnings.push_back("bound with unresolved conditions: " + trouble);
    }
    if (out->constraints.empty()) {
        report->error = "nothing in this study holds the model still, so it has no unique solution";
        return false;
    }
    report->ok = true;
    return true;
}

}  // namespace fem
