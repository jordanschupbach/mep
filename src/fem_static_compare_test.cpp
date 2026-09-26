// Part H.6: the same model solved by somebody else's solver.
//
// The manufactured-solution study says this solver converges to the right
// answer at the right rate, which is the strongest thing a code can say
// about itself. It cannot say anything about the conventions it shares
// with the rest of the world: an element node ordering, a Voigt ordering,
// a sign on a prescribed displacement. Those are all self-consistent
// here, and a self-consistent convention that nobody else uses is a code
// that produces right answers to models nobody else can read.
//
// So the identical mesh, with the identical loads and constraints, goes to
// CalculiX -- a free solver that reads Abaqus .inp -- and the
// displacements are compared node by node. It needs CalculiX, so it takes
// the path to it as an argument rather than searching: a test that quietly
// skips when a tool is missing is a test that quietly stops testing.

#include "fem_static.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <map>
#include <sstream>
#include <string>
#include <vector>

namespace {

int failures = 0;
int checks = 0;

void Check(bool ok, const char *what, int line) {
    ++checks;
    if (ok) return;
    std::fflush(stdout);
    std::printf("CHECK FAILED: %s at %s:%d\n", what, __FILE__, line);
    ++failures;
}

#define CHECK(x) Check((x), #x, __LINE__)
#define CHECK_MESSAGE(x, message) Check((x), (std::string(#x) + ": " + (message)).c_str(), __LINE__)

using cad::Vec3d;
using fem::AnalysisModel;
using fem::BoundElement;
using fem::ElementShape;
using fem::StaticResult;

std::size_t Idx(int i) { return static_cast<std::size_t>(i); }

std::string directory;
std::string calculix;

const char *CalculixType(ElementShape shape) {
    switch (shape) {
        case ElementShape::Hex8: return "C3D8";
        case ElementShape::Tet4: return "C3D4";
        case ElementShape::Tet10: return "C3D10";
        case ElementShape::Hex20: return "C3D20";
        case ElementShape::Wedge6: return "C3D6";
        case ElementShape::Wedge15: return "C3D15";
        default: return nullptr;
    }
}

// THE NODE ORDERINGS AGREE, AND THAT IS WORTH SAYING RATHER THAN
// ASSUMING. CalculiX's C3D8 is the standard isoparametric hexahedron,
// the same one fem_elem.h documents. Its C3D10 numbers the mid-side
// nodes 5..10 as the middles of edges 1-2, 2-3, 3-1, 1-4, 2-4, 3-4,
// which in zero-based terms is (0,1), (1,2), (2,0), (0,3), (1,3), (2,3)
// -- exactly the order fem_elem.h uses, third edge included. If they had
// not agreed, this test would have been the thing that found out.
bool WriteInput(const AnalysisModel &model, const std::string &path, std::string *error) {
    std::ofstream out(path);
    if (!out) {
        *error = "cannot write " + path;
        return false;
    }
    out << "** written by mep for a cross-check\n";
    out << "*NODE, NSET=Nall\n";
    out.precision(17);
    for (int node = 0; node < model.NodeCount(); ++node) {
        const Vec3d &p = model.nodes[Idx(node)];
        out << (node + 1) << ", " << p.x << ", " << p.y << ", " << p.z << "\n";
    }
    // Grouped by element type, since one *ELEMENT block carries one type.
    std::map<std::string, std::vector<int>> by_type;
    for (int e = 0; e < model.ElementCount(); ++e) {
        const char *type = CalculixType(model.elements[Idx(e)].shape);
        if (type == nullptr) {
            *error = std::string("CalculiX has no element matching ") +
                     ElementShapeName(model.elements[Idx(e)].shape);
            return false;
        }
        by_type[type].push_back(e);
    }
    for (const auto &entry : by_type) {
        out << "*ELEMENT, TYPE=" << entry.first << ", ELSET=E" << entry.first << "\n";
        for (const int e : entry.second) {
            out << (e + 1);
            for (const int node : model.elements[Idx(e)].nodes) out << ", " << (node + 1);
            out << "\n";
        }
    }
    out << "*MATERIAL, NAME=MEPMAT\n*ELASTIC\n";
    out << model.materials[0].youngs_modulus.At(model.temperature) << ", "
        << model.materials[0].poissons_ratio.At(model.temperature) << "\n";
    for (const auto &entry : by_type) {
        out << "*SOLID SECTION, ELSET=E" << entry.first << ", MATERIAL=MEPMAT\n";
    }
    out << "*STEP\n*STATIC\n";
    out << "*BOUNDARY\n";
    for (const fem::Constraint &constraint : model.constraints) {
        for (int axis = 0; axis < 3; ++axis) {
            if (!constraint.fixed[axis]) continue;
            out << (constraint.node + 1) << ", " << (axis + 1) << ", " << (axis + 1) << ", "
                << constraint.value[axis] << "\n";
        }
    }
    if (!model.loads.empty()) {
        out << "*CLOAD\n";
        for (const fem::NodalLoad &load : model.loads) {
            const double component[3] = {load.force.x, load.force.y, load.force.z};
            for (int axis = 0; axis < 3; ++axis) {
                if (component[axis] == 0.0) continue;
                out << (load.node + 1) << ", " << (axis + 1) << ", " << component[axis] << "\n";
            }
        }
    }
    // Printed to the .dat rather than written to the .frd: the .dat is
    // line-oriented and this only wants displacements.
    out << "*NODE PRINT, NSET=Nall\nU\n*END STEP\n";
    return true;
}

// The displacements out of CalculiX's .dat.
bool ReadDisplacements(const std::string &path, int nodes, std::vector<Vec3d> *out,
                       std::string *error) {
    std::ifstream in(path);
    if (!in) {
        *error = "cannot read " + path;
        return false;
    }
    out->assign(Idx(nodes), Vec3d{});
    std::string line;
    bool inside = false;
    int seen = 0;
    while (std::getline(in, line)) {
        if (line.find("displacements") != std::string::npos) {
            inside = true;
            continue;
        }
        if (!inside) continue;
        std::istringstream parse(line);
        int node = 0;
        double x = 0.0;
        double y = 0.0;
        double z = 0.0;
        if (!(parse >> node >> x >> y >> z)) {
            if (seen > 0) inside = false;
            continue;
        }
        if (node < 1 || node > nodes) continue;
        (*out)[Idx(node - 1)] = Vec3d{x, y, z};
        ++seen;
    }
    if (seen != nodes) {
        *error = "CalculiX reported " + std::to_string(seen) + " displacements for " +
                 std::to_string(nodes) + " nodes";
        return false;
    }
    return true;
}

bool RunCalculix(const AnalysisModel &model, const std::string &name, std::vector<Vec3d> *out,
                 std::string *error) {
    const std::string stem = directory + "/" + name;
    if (!WriteInput(model, stem + ".inp", error)) return false;
    const std::string command = "cd '" + directory + "' && '" + calculix + "' -i '" + name +
                                "' > '" + name + ".log' 2>&1";
    if (std::system(command.c_str()) != 0) {
        *error = "CalculiX would not run; see " + stem + ".log";
        return false;
    }
    return ReadDisplacements(stem + ".dat", model.NodeCount(), out, error);
}

// --- The model both solvers get -------------------------------------------

AnalysisModel Cantilever(ElementShape shape, int m) {
    AnalysisModel model;
    fem::StudyMaterial material;
    material.youngs_modulus = fem::MaterialCurve::Constant(210e9);
    material.poissons_ratio = fem::MaterialCurve::Constant(0.3);
    model.materials.push_back(material);

    const Vec3d size{0.4, 0.05, 0.1};
    const int nx = m * 4;
    const int ny = std::max(1, m);
    const int nz = std::max(1, m * 2);
    for (int k = 0; k <= nz; ++k) {
        for (int j = 0; j <= ny; ++j) {
            for (int i = 0; i <= nx; ++i) {
                model.nodes.push_back(Vec3d{size.x * i / nx, size.y * j / ny, size.z * k / nz});
            }
        }
    }
    auto at = [&](int i, int j, int k) { return (k * (ny + 1) + j) * (nx + 1) + i; };
    for (int k = 0; k < nz; ++k) {
        for (int j = 0; j < ny; ++j) {
            for (int i = 0; i < nx; ++i) {
                const int h[8] = {at(i, j, k),         at(i + 1, j, k),
                                  at(i + 1, j + 1, k), at(i, j + 1, k),
                                  at(i, j, k + 1),     at(i + 1, j, k + 1),
                                  at(i + 1, j + 1, k + 1), at(i, j + 1, k + 1)};
                if (shape == ElementShape::Hex8) {
                    BoundElement element;
                    element.shape = shape;
                    element.nodes = {h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7]};
                    model.elements.push_back(element);
                    continue;
                }
                const int split[6][4] = {{h[0], h[1], h[2], h[6]}, {h[0], h[2], h[3], h[6]},
                                         {h[0], h[3], h[7], h[6]}, {h[0], h[7], h[4], h[6]},
                                         {h[0], h[4], h[5], h[6]}, {h[0], h[5], h[1], h[6]}};
                for (const auto &tet : split) {
                    BoundElement element;
                    element.shape = ElementShape::Tet4;
                    element.nodes = {tet[0], tet[1], tet[2], tet[3]};
                    model.elements.push_back(element);
                }
            }
        }
    }
    if (shape == ElementShape::Tet10) {
        static const int edges[6][2] = {{0, 1}, {1, 2}, {0, 2}, {0, 3}, {1, 3}, {2, 3}};
        std::map<std::pair<int, int>, int> middles;
        for (BoundElement &element : model.elements) {
            element.shape = ElementShape::Tet10;
            const std::vector<int> corners = element.nodes;
            for (const auto &edge : edges) {
                const int u = corners[Idx(edge[0])];
                const int v = corners[Idx(edge[1])];
                const std::pair<int, int> key =
                    u < v ? std::make_pair(u, v) : std::make_pair(v, u);
                const auto found = middles.find(key);
                if (found != middles.end()) {
                    element.nodes.push_back(found->second);
                    continue;
                }
                const int index = model.NodeCount();
                model.nodes.push_back((model.nodes[Idx(u)] + model.nodes[Idx(v)]) * 0.5);
                middles[key] = index;
                element.nodes.push_back(index);
            }
        }
    }

    for (int node = 0; node < model.NodeCount(); ++node) {
        if (model.nodes[Idx(node)].x != 0.0) continue;
        fem::Constraint constraint;
        constraint.node = node;
        for (int axis = 0; axis < 3; ++axis) constraint.fixed[axis] = true;
        model.constraints.push_back(constraint);
    }
    std::vector<int> tip;
    for (int node = 0; node < model.NodeCount(); ++node) {
        if (model.nodes[Idx(node)].x == size.x) tip.push_back(node);
    }
    for (const int node : tip) {
        model.loads.push_back(
            fem::NodalLoad{node, Vec3d{0, 0, -1e4 / static_cast<double>(tip.size())}});
    }
    return model;
}

void Compare(const char *name, ElementShape shape, int m) {
    const AnalysisModel model = Cantilever(shape, m);
    StaticResult mine;
    fem::StaticOptions options;
    CHECK_MESSAGE(fem::SolveStatic(model, {}, options, &mine), mine.error);
    if (!mine.ok) return;

    std::vector<Vec3d> theirs;
    std::string error;
    if (!RunCalculix(model, name, &theirs, &error)) {
        std::printf("  %-8s CalculiX: %s\n", name, error.c_str());
        ++failures;
        return;
    }

    double worst = 0.0;
    double scale = 0.0;
    for (int node = 0; node < model.NodeCount(); ++node) {
        worst = std::max(worst, (mine.displacement[Idx(node)] - theirs[Idx(node)]).Length());
        scale = std::max(scale, theirs[Idx(node)].Length());
    }
    std::printf("  %-8s %5d nodes %6d elements: tip %.6e vs %.6e, worst node differs by %.2e "
                "(%.1e relative)\n",
                name, model.NodeCount(), model.ElementCount(), mine.max_displacement, scale, worst,
                scale > 0.0 ? worst / scale : 0.0);

    CHECK(scale > 0.0);
    // TWO SOLVERS, ONE MESH, AND THE SAME ANSWER. Not to round-off -- they
    // order their arithmetic differently and CalculiX prints six digits --
    // but to far closer than any modelling difference would give. A node
    // ordering that disagreed, or a sign on a prescribed displacement,
    // would show here as a large difference and nowhere else.
    CHECK(worst < scale * 1e-4);
    CHECK(std::fabs(mine.max_displacement - scale) < scale * 1e-4);
}

}  // namespace

int main(int argc, char **argv) {
    if (argc < 2) {
        std::printf("usage: mep-fem-static-compare-test /path/to/ccx [scratch directory]\n");
        return 2;
    }
    calculix = argv[1];
    directory = argc > 2 ? argv[2] : ".";
    std::printf("the same mesh, solved here and by CalculiX:\n");
    Compare("hex8", ElementShape::Hex8, 2);
    Compare("tet4", ElementShape::Tet4, 2);
    Compare("tet10", ElementShape::Tet10, 1);
    if (failures != 0) {
        std::printf("fem_static_compare_test FAILED (%d of %d checks)\n", failures, checks);
        return 1;
    }
    std::printf("fem_static_compare_test passed (%d checks)\n", checks);
    return 0;
}
