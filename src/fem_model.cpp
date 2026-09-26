#include "fem_model.h"

#include "json.h"

#include <algorithm>
#include <cmath>

namespace fem {
namespace {

inline std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

// The eight corners of the isoparametric reference hexahedron, in the
// node order Element documents. Shared with fem_solve.cpp's shape
// functions through that documented convention rather than a header
// constant, since the two need it in different forms.
constexpr double kHexCorner[8][3] = {{-1, -1, -1}, {1, -1, -1}, {1, 1, -1}, {-1, 1, -1},
                                     {-1, -1, 1},  {1, -1, 1},  {1, 1, 1},  {-1, 1, 1}};

// Jacobian determinant of a Hex8 at one reference point -- duplicated
// here (fem_solve.cpp has the full shape-function machinery) because
// Validate must be usable without pulling in the solver, and because
// this only needs the determinant, not the inverse or the B matrix.
double HexJacobianDeterminant(const Model &model, const Element &element, double xi, double eta, double zeta) {
    // dN/dxi, dN/deta, dN/dzeta for the trilinear shape functions.
    double jacobian[3][3] = {{0, 0, 0}, {0, 0, 0}, {0, 0, 0}};
    for (int n = 0; n < 8; ++n) {
        const double sx = kHexCorner[n][0];
        const double sy = kHexCorner[n][1];
        const double sz = kHexCorner[n][2];
        const double dn[3] = {0.125 * sx * (1.0 + sy * eta) * (1.0 + sz * zeta),
                              0.125 * (1.0 + sx * xi) * sy * (1.0 + sz * zeta),
                              0.125 * (1.0 + sx * xi) * (1.0 + sy * eta) * sz};
        const cad::Vec3d &p = model.nodes[Idx(element.nodes[static_cast<std::size_t>(n)])];
        for (int r = 0; r < 3; ++r) {
            jacobian[r][0] += dn[r] * p.x;
            jacobian[r][1] += dn[r] * p.y;
            jacobian[r][2] += dn[r] * p.z;
        }
    }
    return jacobian[0][0] * (jacobian[1][1] * jacobian[2][2] - jacobian[1][2] * jacobian[2][1]) -
           jacobian[0][1] * (jacobian[1][0] * jacobian[2][2] - jacobian[1][2] * jacobian[2][0]) +
           jacobian[0][2] * (jacobian[1][0] * jacobian[2][1] - jacobian[1][1] * jacobian[2][0]);
}

}  // namespace

bool Material::IsValid(std::string *error) const {
    if (!(youngs_modulus > 0.0)) {
        *error = "Young's modulus must be positive (got " + std::to_string(youngs_modulus) + ")";
        return false;
    }
    if (!(poissons_ratio > -1.0 && poissons_ratio < 0.5)) {
        *error = "Poisson's ratio must lie in (-1, 0.5), exclusive (got " + std::to_string(poissons_ratio) +
                 "); 0.5 exactly is incompressible and needs a mixed formulation this solver does not have";
        return false;
    }
    if (!(density >= 0.0)) {
        *error = "density must not be negative";
        return false;
    }
    return true;
}

bool Model::Validate(std::string *error) const {
    if (nodes.empty()) {
        *error = "model has no nodes";
        return false;
    }
    if (elements.empty()) {
        *error = "model has no elements";
        return false;
    }
    if (!material.IsValid(error)) return false;

    const int node_count = NodeCount();
    for (std::size_t e = 0; e < elements.size(); ++e) {
        for (int n = 0; n < 8; ++n) {
            const int index = elements[e].nodes[static_cast<std::size_t>(n)];
            if (index < 0 || index >= node_count) {
                *error = "element " + std::to_string(e) + " references node " + std::to_string(index) +
                         ", which does not exist";
                return false;
            }
        }
        // Check the Jacobian at all eight corners, not just the centre: a
        // badly-shaped hex can be positive at the centre and inverted in a
        // corner, and an element that is inverted anywhere contributes a
        // wrong (and possibly negative-definite) stiffness.
        for (int c = 0; c < 8; ++c) {
            const double det =
                HexJacobianDeterminant(*this, elements[e], kHexCorner[c][0], kHexCorner[c][1], kHexCorner[c][2]);
            if (det <= 0.0) {
                *error = "element " + std::to_string(e) + " has a non-positive Jacobian (" + std::to_string(det) +
                         ") at corner " + std::to_string(c) +
                         "; it is inverted or degenerate, most often from nodes listed in the wrong order";
                return false;
            }
        }
    }

    for (const Constraint &c : constraints) {
        if (c.node < 0 || c.node >= node_count) {
            *error = "constraint references node " + std::to_string(c.node) + ", which does not exist";
            return false;
        }
    }
    for (const NodalLoad &l : loads) {
        if (l.node < 0 || l.node >= node_count) {
            *error = "load references node " + std::to_string(l.node) + ", which does not exist";
            return false;
        }
    }

    // Six rigid-body modes have to be removed for a static solve to have
    // a unique answer. Counting fixed axes is a necessary condition, not
    // a sufficient one (six constraints all on one node still leave the
    // body free to rotate about it), so the factorization's own
    // zero-pivot detection remains the real check -- this one exists to
    // turn the most common mistake into a clear message instead.
    int fixed_axes = 0;
    for (const Constraint &c : constraints) {
        for (int a = 0; a < 3; ++a) {
            if (c.fixed[a]) ++fixed_axes;
        }
    }
    if (fixed_axes < 6) {
        *error = "model has only " + std::to_string(fixed_axes) +
                 " fixed degrees of freedom; at least 6 are needed to remove the rigid-body modes, or the "
                 "stiffness matrix is singular";
        return false;
    }
    return true;
}

cad::Box3d Model::BoundingBox() const {
    cad::Box3d box;
    for (const cad::Vec3d &p : nodes) box.Expand(p);
    return box;
}

Model MakeBoxMesh(const cad::Vec3d &min_corner, const cad::Vec3d &size, const std::array<int, 3> &divisions,
                  const Material &material) {
    Model model;
    model.material = material;
    const int nx = std::max(1, divisions[0]);
    const int ny = std::max(1, divisions[1]);
    const int nz = std::max(1, divisions[2]);

    model.nodes.reserve(Idx((nx + 1) * (ny + 1) * (nz + 1)));
    for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                model.nodes.push_back(cad::Vec3d{
                    min_corner.x + size.x * static_cast<double>(i) / static_cast<double>(nx),
                    min_corner.y + size.y * static_cast<double>(j) / static_cast<double>(ny),
                    min_corner.z + size.z * static_cast<double>(k) / static_cast<double>(nz)});
            }
        }
    }

    auto node_at = [nx, ny](int i, int j, int k) { return i + (nx + 1) * (j + (ny + 1) * k); };
    model.elements.reserve(Idx(nx * ny * nz));
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                Element e;
                // The isoparametric order Element documents: the -zeta
                // face first, counter-clockwise seen from +zeta, then the
                // +zeta face in the same rotational order. Getting this
                // wrong inverts the element, which Validate catches.
                e.nodes[0] = node_at(i, j, k);
                e.nodes[1] = node_at(i + 1, j, k);
                e.nodes[2] = node_at(i + 1, j + 1, k);
                e.nodes[3] = node_at(i, j + 1, k);
                e.nodes[4] = node_at(i, j, k + 1);
                e.nodes[5] = node_at(i + 1, j, k + 1);
                e.nodes[6] = node_at(i + 1, j + 1, k + 1);
                e.nodes[7] = node_at(i, j + 1, k + 1);
                model.elements.push_back(e);
            }
        }
    }
    return model;
}

std::vector<int> BoxMeshFaceNodes(const std::array<int, 3> &divisions, int axis, bool max_side) {
    std::vector<int> result;
    if (axis < 0 || axis > 2) return result;
    const int nx = std::max(1, divisions[0]);
    const int ny = std::max(1, divisions[1]);
    const int nz = std::max(1, divisions[2]);
    const int counts[3] = {nx, ny, nz};
    const int target = max_side ? counts[axis] : 0;
    for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                const int coordinate[3] = {i, j, k};
                if (coordinate[axis] != target) continue;
                result.push_back(i + (nx + 1) * (j + (ny + 1) * k));
            }
        }
    }
    return result;
}

// ---------------------------------------------------------------------
// JSON
// ---------------------------------------------------------------------

std::string ModelToJson(const Model &model) {
    Json root;
    // Flat coordinate and connectivity arrays rather than an array of
    // objects: for a mesh of any size the difference in both encode time
    // and message size is large, and neither end ever reads this by
    // random access.
    Json nodes;
    for (const cad::Vec3d &p : model.nodes) {
        nodes.push_back(Json(p.x));
        nodes.push_back(Json(p.y));
        nodes.push_back(Json(p.z));
    }
    root["nodes"] = nodes;

    Json elements;
    for (const Element &e : model.elements) {
        for (int n = 0; n < 8; ++n) elements.push_back(Json(static_cast<double>(e.nodes[static_cast<std::size_t>(n)])));
    }
    root["hexes"] = elements;

    Json material;
    material["name"] = Json(model.material.name);
    material["youngs_modulus"] = Json(model.material.youngs_modulus);
    material["poissons_ratio"] = Json(model.material.poissons_ratio);
    material["density"] = Json(model.material.density);
    root["material"] = material;

    Json constraints;
    for (const Constraint &c : model.constraints) {
        Json item;
        item["node"] = Json(static_cast<double>(c.node));
        Json fixed;
        Json value;
        for (int a = 0; a < 3; ++a) {
            fixed.push_back(Json(c.fixed[a]));
            value.push_back(Json(c.value[a]));
        }
        item["fixed"] = fixed;
        item["value"] = value;
        constraints.push_back(item);
    }
    root["constraints"] = constraints;

    Json loads;
    for (const NodalLoad &l : model.loads) {
        Json item;
        item["node"] = Json(static_cast<double>(l.node));
        Json force;
        force.push_back(Json(l.force.x));
        force.push_back(Json(l.force.y));
        force.push_back(Json(l.force.z));
        item["force"] = force;
        loads.push_back(item);
    }
    root["loads"] = loads;
    return root.dump();
}

bool ModelFromJson(const std::string &text, Model *out, std::string *error) {
    Json root;
    if (!Json::Parse(text, &root)) {
        *error = "model is not valid JSON";
        return false;
    }
    Model model;

    const Json &nodes = root.get("nodes");
    if (!nodes.is_array() || nodes.size() % 3 != 0) {
        *error = "\"nodes\" must be an array of x,y,z triples";
        return false;
    }
    const std::vector<Json> &node_items = nodes.items();
    model.nodes.reserve(node_items.size() / 3);
    for (std::size_t i = 0; i + 2 < node_items.size(); i += 3) {
        model.nodes.push_back(
            cad::Vec3d{node_items[i].as_double(), node_items[i + 1].as_double(), node_items[i + 2].as_double()});
    }

    const Json &hexes = root.get("hexes");
    if (!hexes.is_array() || hexes.size() % 8 != 0) {
        *error = "\"hexes\" must be an array of 8-node connectivity records";
        return false;
    }
    const std::vector<Json> &hex_items = hexes.items();
    model.elements.reserve(hex_items.size() / 8);
    for (std::size_t i = 0; i + 7 < hex_items.size(); i += 8) {
        Element e;
        for (int n = 0; n < 8; ++n) {
            e.nodes[static_cast<std::size_t>(n)] = hex_items[i + static_cast<std::size_t>(n)].as_int();
        }
        model.elements.push_back(e);
    }

    const Json &material = root.get("material");
    model.material.name = material.get("name").as_string("default");
    // Defaults rather than errors for a missing material: a request that
    // omits it means "use structural steel", which is far more often what
    // was intended than a typo worth rejecting the whole solve over.
    model.material.youngs_modulus = material.get("youngs_modulus").as_double(210e9);
    model.material.poissons_ratio = material.get("poissons_ratio").as_double(0.3);
    model.material.density = material.get("density").as_double(7850.0);

    const Json &constraints = root.get("constraints");
    if (constraints.is_array()) {
        for (const Json &item : constraints.items()) {
            Constraint c;
            c.node = item.get("node").as_int(-1);
            const std::vector<Json> &fixed = item.get("fixed").items();
            const std::vector<Json> &value = item.get("value").items();
            for (int a = 0; a < 3; ++a) {
                const std::size_t ai = static_cast<std::size_t>(a);
                if (ai < fixed.size()) c.fixed[a] = fixed[ai].as_bool();
                if (ai < value.size()) c.value[a] = value[ai].as_double();
            }
            model.constraints.push_back(c);
        }
    }

    const Json &loads = root.get("loads");
    if (loads.is_array()) {
        for (const Json &item : loads.items()) {
            NodalLoad l;
            l.node = item.get("node").as_int(-1);
            const std::vector<Json> &force = item.get("force").items();
            if (force.size() >= 3) {
                l.force = cad::Vec3d{force[0].as_double(), force[1].as_double(), force[2].as_double()};
            }
            model.loads.push_back(l);
        }
    }

    *out = std::move(model);
    return true;
}

}  // namespace fem
