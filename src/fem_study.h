#ifndef MEP_FEM_STUDY_H
#define MEP_FEM_STUDY_H

#include "cad_naming.h"
#include "cad_topology.h"
#include "fem_elem.h"
#include "fem_mesh.h"
#include "fem_model.h"

#include <string>
#include <vector>

// The analysis model, bound to topology (plans/CAD_FEM_PLAN.md H.2).
//
// THIS IS THE FEATURE THE WHOLE COUPLING EXISTS FOR. Every mesh-file FEM
// tool in the world attaches its boundary conditions to node numbers,
// because a mesh file is all it has. That works exactly once: change a
// dimension, remesh, and the node numbers mean something else, so every
// constraint and every load has to be picked again by hand. It is the
// single largest cost of using such a tool and it is entirely an
// artefact of where the conditions are attached.
//
// Here a condition is attached to a *named* face or edge of the B-rep --
// a name in the Part B.4 sense, which survives a dimension change
// because it records what generated the face and what it is next to,
// not where it happens to be. Change a dimension, rebuild, remesh, and
// the fixed face is still fixed.
//
// AND WHERE A NAME NO LONGER RESOLVES, THAT IS REPORTED RATHER THAN
// GUESSED AT. A face consumed by a boolean is gone, and a condition on
// it cannot be honoured; two faces that score alike cannot be told
// apart. Both come back named, in a report, because a study that
// silently moved a load onto the wrong face would be worse than one that
// refused to run.
namespace fem {

// --- Materials ------------------------------------------------------------

// A property that varies with temperature, as a table read by linear
// interpolation and held flat outside its range.
//
// FLAT OUTSIDE, NOT EXTRAPOLATED. A modulus extrapolated past the last
// point in a table goes negative eventually, and a negative modulus is
// not a warning, it is a solver that produces a confident answer of the
// wrong sign. Holding the end value is wrong too, but wrong in a way
// that cannot invert the physics.
struct MaterialCurve {
    struct Point {
        double temperature = 0.0;
        double value = 0.0;
    };
    std::vector<Point> points;

    // A curve with one point is a constant, which is the common case and
    // should not need a table to express.
    static MaterialCurve Constant(double value);
    double At(double temperature) const;
    // The slope at a temperature: piecewise constant, since the curve is
    // piecewise linear, and zero outside the table because the value is
    // held flat there. Needed by any Newton iteration on a
    // temperature-dependent property -- without it the iteration is a
    // fixed point rather than a Newton step, and converges linearly.
    double Slope(double temperature) const;
    bool IsValid(std::string *error) const;
};

enum class MaterialKind {
    Isotropic,
    // Nine independent constants and a local frame. Wood, laminates,
    // rolled sheet and anything printed have them; treating them as
    // isotropic underestimates deflection along the weak axis by however
    // much the ratio of moduli is.
    Orthotropic,
};

struct StudyMaterial {
    std::string name = "material";
    MaterialKind kind = MaterialKind::Isotropic;

    // Isotropic, possibly temperature-dependent.
    MaterialCurve youngs_modulus = MaterialCurve::Constant(210e9);
    MaterialCurve poissons_ratio = MaterialCurve::Constant(0.3);

    // Orthotropic: moduli along the local axes, the three Poisson's
    // ratios and the three shear moduli. The local frame is given by two
    // directions; the third is their cross product.
    double e[3] = {210e9, 210e9, 210e9};
    double nu[3] = {0.3, 0.3, 0.3};  // nu_xy, nu_yz, nu_zx
    double g[3] = {80.77e9, 80.77e9, 80.77e9};
    cad::Vec3d axis_x{1, 0, 0};
    cad::Vec3d axis_y{0, 1, 0};

    double density = 7850.0;             // kg/m^3
    // 1/K. A curve for the same reason the modulus is one: it varies with
    // temperature, and over the range a thermal-stress problem covers the
    // variation is not small.
    MaterialCurve thermal_expansion = MaterialCurve::Constant(1.2e-5);

    // The 6x6 constitutive matrix in Voigt order, at a temperature.
    bool ConstitutiveMatrix(double temperature, double out[6][6], std::string *error) const;
    // The isotropic view, for the parts of the solver that still take one.
    // Fails for an orthotropic material rather than averaging it, because
    // an averaged orthotropic material is not any material.
    bool AsIsotropic(double temperature, Material *out, std::string *error) const;
};

// --- What a condition is attached to ---------------------------------------

enum class TargetKind { Face, Edge };

struct Target {
    TargetKind kind = TargetKind::Face;
    // The persistent name, which is what makes this survive a rebuild.
    cad::EntityName name;
    // Filled in by binding. Not part of the study: a study saved with a
    // resolved id in it would be a study that had quietly gone back to
    // pointing at a particular build.
    cad::EntityId resolved = cad::kNoEntity;
};

// Captures a target on a face or an edge of the model as it stands.
//
// THE ROLE MUST BE THE ENTITY'S OWN RECORDED NAME, and that is why these
// exist rather than a caller passing a role of its own choosing.
// ResolveFace builds each candidate's name using the candidate face's
// `name` field as its role, so a target captured with any other string
// can never match on that signal -- and the role is a fifth of the whole
// score. Nothing fails when it is wrong; the reference merely resolves
// less certainly, which on a box, whose six faces are alike in every
// other respect, is the difference between resolving and reporting
// ambiguity. It was got wrong here first, and the symptom was a study
// that bound to a block and would not bind to the same block made taller.
Target FaceTarget(const cad::Model &model, cad::EntityId face, int generating_feature = 1);
Target EdgeTarget(const cad::Model &model, cad::EntityId edge, int generating_feature = 1);

struct Support {
    std::string label = "support";
    Target where;
    bool fixed[3] = {true, true, true};
    double value[3] = {0.0, 0.0, 0.0};
};

enum class LoadKind {
    // A total force spread over the target, as a consistent load rather
    // than divided equally between its nodes -- see the note on binding.
    Force,
    // Pressure, positive into the surface. Pa.
    Pressure,
    // Traction: force per unit area, as a vector. Pa.
    Traction,
    // Acceleration applied to the whole model, times density. m/s^2.
    Gravity,
};

struct Load {
    std::string label = "load";
    LoadKind kind = LoadKind::Force;
    Target where;  // ignored for Gravity
    cad::Vec3d vector;
    double magnitude = 0.0;  // pressure only
};

// A material assigned to a body.
struct Section {
    cad::EntityId body = cad::kNoEntity;
    int material = 0;  // index into Study::materials
};

struct Study {
    std::string name = "study";
    std::vector<StudyMaterial> materials;
    std::vector<Section> sections;
    std::vector<Support> supports;
    std::vector<Load> loads;
    double temperature = 20.0;
    double reference_temperature = 20.0;
};

// --- What binding produces -------------------------------------------------

struct BoundElement {
    ElementShape shape = ElementShape::Tet4;
    std::vector<int> nodes;
    int material = 0;
};

// The analysis model proper: a mesh, materials per element, and every
// condition resolved to nodes and nodal forces. This is what Part H.3's
// assembly consumes.
//
// Part 0.6's `fem::Model` is the narrow ancestor of this and still what
// `fem_solve.cpp` takes; it carries Hex8 and nothing else. Retiring it in
// favour of this is H.3's job, because the assembly changes with it.
struct AnalysisModel {
    std::vector<cad::Vec3d> nodes;
    std::vector<BoundElement> elements;
    std::vector<StudyMaterial> materials;
    std::vector<Constraint> constraints;
    std::vector<NodalLoad> loads;
    // The uniform temperature, used when `node_temperature` is empty.
    double temperature = 20.0;
    // A TEMPERATURE PER NODE, WHICH IS WHAT A COUPLING DELIVERS. Empty for
    // an isothermal model. When it is set, each element's constitutive
    // matrix is built at its own mean temperature and the thermal strain is
    // integrated against the field rather than against a constant -- and
    // the difference between those two is the whole of thermal stress,
    // since a uniform temperature on a free body produces none.
    std::vector<double> node_temperature;
    // What the model is stress-free at.
    double reference_temperature = 20.0;

    double MeanTemperature(const std::vector<int> &nodes) const;

    int NodeCount() const { return static_cast<int>(nodes.size()); }
    int ElementCount() const { return static_cast<int>(elements.size()); }
    cad::Vec3d TotalLoad() const;
};

struct BoundCondition {
    std::string label;
    cad::ResolveStatus status = cad::ResolveStatus::Lost;
    cad::EntityId entity = cad::kNoEntity;
    int nodes = 0;
    double area = 0.0;          // for a face target
    cad::Vec3d force;           // what the load came to, for a load
    std::string explanation;
};

struct BindReport {
    bool ok = false;
    std::string error;
    std::vector<std::string> warnings;
    std::vector<BoundCondition> conditions;
    int resolved = 0;
    int ambiguous = 0;
    int lost = 0;
};

// Resolves every condition of `study` against `model`, applies them to
// `mesh`, and writes the analysis model.
//
// Fails, rather than producing a model, when a condition cannot be
// honoured: an unresolved support is a model that is free to fly away,
// and an unresolved load is a model that is not the one that was asked
// for. `options.allow_unresolved` turns those into warnings for a caller
// that would rather see a partial result -- an interactive one, showing
// what is still attached while the user fixes the rest.
struct BindOptions {
    cad::NamingOptions naming;
    bool allow_unresolved = false;
};

bool BindStudy(const cad::Model &model, const VolumeMesh &mesh, const Study &study,
               const BindOptions &options, AnalysisModel *out, BindReport *report);

// The nodes of a mesh that lie on a resolved face or edge. Exposed
// because selecting a set is useful on its own -- highlighting it in the
// viewport is the same question -- and because it is worth testing
// without a study wrapped round it.
std::vector<int> NodesOnFace(const VolumeMesh &mesh, cad::EntityId face);
std::vector<int> NodesOnEdge(const VolumeMesh &mesh, cad::EntityId edge);

// The consistent nodal forces of a uniform traction over a face, and the
// area it acted on.
//
// CONSISTENT, NOT DIVIDED EQUALLY. Splitting a face's total force evenly
// between its nodes is wrong wherever the mesh is graded, and wrong in a
// particular way for second-order elements: the correct share for a
// corner node of a quadratic triangle is *negative*. Dividing equally
// there puts the load in the wrong place and the answer is wrong near the
// surface, which is exactly where the stress is read.
bool FaceTraction(const VolumeMesh &mesh, cad::EntityId face, const cad::Vec3d &traction,
                  double pressure, std::vector<NodalLoad> *out, double *out_area,
                  std::string *error);

}  // namespace fem

#endif
