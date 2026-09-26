// Builds the geometry for the NAFEMS example set: one `.mepcad` feature
// tree per benchmark, written into examples/nafems and opened by the
// editor's CAD pane or by `part.import` on the headless surface.
//
// WHY A GENERATOR AND NOT NINE CHECKED-IN FILES. A `.mepcad` document is
// JSON holding a feature tree, and a feature tree hand-written as JSON is
// a thing nobody can read and nobody dares change: every sketch entity
// refers to its points by id, every constraint refers to entities by id,
// and the whole benchmark is one mistyped integer away from being a
// different part. Here the geometry is the benchmark's own numbers,
// written once, and the ids are the sketch's business.
//
// The files are still checked in -- an example you have to build before
// you can open it is not an example -- and this program is what keeps
// them honest. It rebuilds every tree it writes and reports the volume
// against the closed form, so a geometry that has quietly stopped being
// the benchmark's geometry says so here rather than in a frequency six
// steps later.
//
// EVERY QUARTER MODEL IS MADE THE SAME WAY: the full section, then an
// intersect with a big block covering the quadrant. The alternative --
// sketching the quarter directly -- needs elliptical *arcs*, which the
// sketch has no entity for (it has whole ellipses), and approximating
// one with a spline would put a benchmark's boundary a little way off the
// ellipse it is defined by. An intersect is exact.

#include "cad_doc.h"
#include "cad_feature.h"
#include "cad_sketch.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <string>
#include <vector>

namespace {

using cad::CadDocument;
using cad::Feature;
using cad::FeatureCombine;
using cad::FeatureKind;
using cad::FeatureTree;
using cad::Sketch;
using cad::SketchId;
using cad::SketchPlane;
using cad::Vec2d;
using cad::Vec3d;

constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

int failures = 0;

void Rectangle(Sketch *sketch, const Vec2d &low, const Vec2d &high) {
    const SketchId a = sketch->AddPoint(Vec2d{low.x, low.y});
    const SketchId b = sketch->AddPoint(Vec2d{high.x, low.y});
    const SketchId c = sketch->AddPoint(Vec2d{high.x, high.y});
    const SketchId d = sketch->AddPoint(Vec2d{low.x, high.y});
    sketch->AddLineFromPoints(a, b);
    sketch->AddLineFromPoints(b, c);
    sketch->AddLineFromPoints(c, d);
    sketch->AddLineFromPoints(d, a);
}

void Polygon(Sketch *sketch, const std::vector<Vec2d> &corners) {
    std::vector<SketchId> points;
    points.reserve(corners.size());
    for (const Vec2d &corner : corners) points.push_back(sketch->AddPoint(corner));
    for (std::size_t i = 0; i < points.size(); ++i) {
        sketch->AddLineFromPoints(points[i], points[(i + 1) % points.size()]);
    }
}

// WHICH PROFILE AN EXTRUDE SHOULD USE, when the sketch is two nested
// closed curves.
//
// A Feature's default is -1, "the profile of largest area, which is what
// a single closed outline with holes comes out as". That is true right
// up until the hole is most of the part: extraction offers both the
// annulus and the disc filling its hole, and for FV41's cylinder -- a
// 50 mm wall on a 1 m radius -- the disc is nine times the larger. The
// extrude then quietly produced a solid rod where a tube was asked for,
// and the first sign of it was a volume.
//
// So the profile is chosen by what it *is* rather than by how big it is:
// the one with a hole in it.
int ProfileWithAHole(const Sketch &sketch) {
    std::vector<Sketch::Profile> profiles;
    std::string error;
    if (!sketch.ExtractProfiles(&profiles, &error)) return -1;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        if (!profiles[i].holes.empty()) return static_cast<int>(i);
    }
    return -1;
}

// The quadrant selector: a block covering x >= 0, y >= 0 and the whole
// thickness, intersected with whatever came before.
void AddQuadrant(FeatureTree *tree, double reach) {
    Sketch selector;
    Rectangle(&selector, Vec2d{0.0, 0.0}, Vec2d{reach, reach});
    const int id = tree->AddSketch(selector, "quadrant");
    Feature cut;
    cut.kind = FeatureKind::Extrude;
    cut.name = "quarter";
    cut.sketch = id;
    cut.end = cad::ExtrudeEnd::Symmetric;
    cut.distance = reach;
    cut.combine = FeatureCombine::Intersect;
    tree->AddFeature(cut);
}

bool Write(const std::string &directory, const std::string &name, const std::string &title,
           const std::string &notes, FeatureTree tree, double expected_volume) {
    std::string error;
    if (!tree.Rebuild(&error)) {
        std::printf("  %-8s REBUILD FAILED: %s\n", name.c_str(), error.c_str());
        ++failures;
        return false;
    }
    const double volume = tree.Volume();
    const double slip = expected_volume > 0.0 ? std::fabs(volume - expected_volume) / expected_volume
                                              : 0.0;
    CadDocument document;
    document.title = title;
    document.notes = notes;
    document.tree = tree;
    const std::string path = directory + "/" + name + ".mepcad";
    std::ofstream file(path, std::ios::binary);
    if (!file) {
        std::printf("  %-8s cannot write %s\n", name.c_str(), path.c_str());
        ++failures;
        return false;
    }
    file << cad::WriteCadDocument(document);
    for (const cad::Feature &feature : tree.Features()) {
        const cad::FeatureResult *result = tree.ResultOf(feature.id);
        if (result == nullptr || feature.kind == cad::FeatureKind::Sketch) continue;
        std::printf("  %-8s   after '%s': %d bod%s, %.6g m3\n", name.c_str(),
                    feature.name.c_str(), static_cast<int>(result->bodies.size()),
                    result->bodies.size() == 1 ? "y" : "ies", result->volume);
    }
    std::printf("  %-8s %2d features, %d bod%s, volume %.6g m3 (%.6g expected, %.3f%% apart)\n",
                name.c_str(), static_cast<int>(tree.Features().size()),
                static_cast<int>(tree.Bodies().size()), tree.Bodies().size() == 1 ? "y" : "ies",
                volume, expected_volume, slip * 100.0);
    // A percent is generous for a volume and tight for a tessellated one:
    // the ellipses and arcs here are measured by facets, so exact
    // agreement is not on offer and a disagreement of a percent is a
    // geometry that is not the benchmark's.
    if (slip > 0.01) {
        std::printf("  %-8s VOLUME IS WRONG\n", name.c_str());
        ++failures;
    }
    return true;
}

// --- The quarter elliptic annulus, shared by LE1 and LE10 -----------------
double QuarterEllipticAnnulusArea() {
    return 0.25 * kPi * (3.25 * 2.75 - 2.00 * 1.00);
}

// The index of the profile whose area is nearest `want`.
int ProfileByArea(const Sketch &sketch, double want) {
    std::vector<Sketch::Profile> profiles;
    std::string error;
    if (!sketch.ExtractProfiles(&profiles, &error)) return -1;
    int best = -1;
    double nearest = 0.0;
    for (std::size_t i = 0; i < profiles.size(); ++i) {
        const double apart = std::fabs(std::fabs(profiles[i].area) - want);
        if (best < 0 || apart < nearest) {
            best = static_cast<int>(i);
            nearest = apart;
        }
    }
    return best;
}

FeatureTree EllipticPlate(double thickness, bool symmetric) {
    FeatureTree tree;
    Sketch plan;
    // Outer first: ExtractProfiles takes the largest-area loop as the
    // outer one and everything inside it as a hole, so the order here is
    // documentation rather than instruction.
    // THE QUARTER IS SKETCHED, NOT CUT OUT, and this is the one place in
    // the set where that distinction cost anything.
    //
    // Every other quarter model here is the full section intersected with
    // a block over the quadrant, which is exact and reads well. For the
    // ellipses it does not work: the boolean comes back with an open
    // shell -- a dozen edges each used by a single coedge -- wherever the
    // cutting plane has to trim an elliptical lateral face. Moving the
    // ellipse's seam out of the way (a half turn, which leaves an ellipse
    // exactly where it was) does not help, so it is the trimming of the
    // face and not the seam. That is a real limitation of the boolean and
    // it is written up in plans/NAFEMS_PLAN.md rather than worked around
    // silently.
    //
    // What works instead is to let the *sketch* do it. Two whole ellipses
    // and two line segments along the axes make an arrangement with three
    // bounded faces -- the hole, the quadrant, and the other three
    // quadrants -- and profile extraction already cuts entities where
    // other curves cross them. The quadrant is picked by its area, which
    // is a number the benchmark itself gives.
    plan.AddEllipse(Vec2d{0.0, 0.0}, 3.25, 2.75, 0.0);
    plan.AddEllipse(Vec2d{0.0, 0.0}, 2.00, 1.00, 0.0);
    plan.AddLine(Vec2d{2.00, 0.0}, Vec2d{3.25, 0.0});
    plan.AddLine(Vec2d{0.0, 1.00}, Vec2d{0.0, 2.75});
    const int sketch = tree.AddSketch(plan, "ellipses");
    Feature plate;
    plate.kind = FeatureKind::Extrude;
    plate.name = "plate";
    plate.sketch = sketch;
    plate.profile_index = ProfileByArea(plan, QuarterEllipticAnnulusArea());
    plate.end = symmetric ? cad::ExtrudeEnd::Symmetric : cad::ExtrudeEnd::Blind;
    plate.distance = thickness;
    tree.AddFeature(plate);
    return tree;
}

// --- A quarter tube, shared by LE7 and FV41 -------------------------------
FeatureTree Tube(double inner, double outer, double length, bool centred) {
    FeatureTree tree;
    Sketch section;
    section.AddCircle(Vec2d{0.0, 0.0}, outer);
    section.AddCircle(Vec2d{0.0, 0.0}, inner);
    const int sketch = tree.AddSketch(section, "section");
    Feature tube;
    tube.kind = FeatureKind::Extrude;
    tube.name = "tube";
    tube.sketch = sketch;
    tube.profile_index = ProfileWithAHole(section);
    tube.end = centred ? cad::ExtrudeEnd::Symmetric : cad::ExtrudeEnd::Blind;
    tube.distance = length;
    tree.AddFeature(tube);
    AddQuadrant(&tree, std::max(outer, length) * 3.0);
    return tree;
}

// --- A quarter hollow sphere, revolved ------------------------------------
FeatureTree HollowSphere(double inner, double outer) {
    FeatureTree tree;
    // A plane containing the axis: sketch x is world x, sketch y is world
    // z. A revolve's profile has to lie in such a plane, and the axis has
    // to be a construction line drawn in it -- which is what keeps the
    // axis parametric rather than a pair of numbers nobody can find
    // again.
    SketchPlane plane;
    plane.origin = Vec3d{0.0, 0.0, 0.0};
    plane.x_axis = Vec3d{1.0, 0.0, 0.0};
    plane.y_axis = Vec3d{0.0, 0.0, 1.0};
    Sketch profile(plane);
    const SketchId axis_start = profile.AddPoint(Vec2d{0.0, -outer * 2.0});
    const SketchId axis_end = profile.AddPoint(Vec2d{0.0, outer * 2.0});
    const SketchId axis = profile.AddLineFromPoints(axis_start, axis_end, true);
    // Clockwise from the north pole, so the arc goes out through the
    // equator rather than the wrong way round the back.
    profile.AddArc(Vec2d{0.0, 0.0}, Vec2d{0.0, outer}, Vec2d{0.0, -outer}, false);
    profile.AddArc(Vec2d{0.0, 0.0}, Vec2d{0.0, inner}, Vec2d{0.0, -inner}, false);
    const SketchId top_inner = profile.AddPoint(Vec2d{0.0, inner});
    const SketchId top_outer = profile.AddPoint(Vec2d{0.0, outer});
    const SketchId bottom_inner = profile.AddPoint(Vec2d{0.0, -inner});
    const SketchId bottom_outer = profile.AddPoint(Vec2d{0.0, -outer});
    profile.AddLineFromPoints(top_inner, top_outer);
    profile.AddLineFromPoints(bottom_inner, bottom_outer);
    const int sketch = tree.AddSketch(profile, "profile");
    Feature shell;
    shell.kind = FeatureKind::Revolve;
    shell.name = "shell";
    shell.sketch = sketch;
    shell.axis_entity = axis;
    shell.angle = kTwoPi * 0.25;
    tree.AddFeature(shell);
    return tree;
}

FeatureTree Prism(const std::vector<Vec2d> &plan, double thickness, bool symmetric) {
    FeatureTree tree;
    Sketch outline;
    Polygon(&outline, plan);
    const int sketch = tree.AddSketch(outline, "outline");
    Feature solid;
    solid.kind = FeatureKind::Extrude;
    solid.name = "solid";
    solid.sketch = sketch;
    solid.end = symmetric ? cad::ExtrudeEnd::Symmetric : cad::ExtrudeEnd::Blind;
    solid.distance = thickness;
    tree.AddFeature(solid);
    return tree;
}

}  // namespace

int main(int argc, char **argv) {
    const std::string root = argc > 1 ? argv[1] : ".";
    const std::string directory = root + "/examples/nafems";
    std::printf("writing the NAFEMS example geometry into %s\n", directory.c_str());

    // LE1: the elliptic membrane. Modelled as HALF the thickness, from
    // the mid-plane up, because the mid-plane is where the plane-stress
    // slab is held: a plane-stress state has u_z proportional to z, so it
    // is already zero there, and holding it there removes the rigid
    // motion without restraining anything. Held on the *faces* it would
    // be plane strain, which is a different problem.
    Write(directory, "le1", "NAFEMS LE1 -- elliptic membrane (quarter, half thickness)",
          "Quarter of an elliptic annulus, 0.05 m of a 0.1 m slab. The cut at z = 0 is the"
          " mid-plane and is where u_z is held. Outer edge carries 10 MPa outward.",
          EllipticPlate(0.05, false), QuarterEllipticAnnulusArea() * 0.05);

    // LE10: the same plan, 0.6 m thick, modelled through its full
    // thickness because the load is on the face and the answer is a
    // through-thickness bending stress.
    Write(directory, "le10", "NAFEMS LE10 -- thick elliptic plate (quarter)",
          "Quarter of an elliptic annulus 0.6 m thick, 1 MPa on the upper face. The target is"
          " sigma_yy at D = (3.25, 0, 0.3).",
          EllipticPlate(0.6, true), QuarterEllipticAnnulusArea() * 0.6);

    // LE7: a thick cylinder under internal pressure. The benchmark's own
    // part is a cylinder capped by a hemisphere; this is the cylinder,
    // which is the half that has Lame's closed form at every point.
    Write(directory, "le7", "NAFEMS LE7 -- thick cylinder (quarter sector)",
          "Bore 1 m, outside 2 m, 10 MPa internal with the closed-end axial traction applied as"
          " well, so Lame holds at every node. Half the length is modelled, from the"
          " mid-length plane at z = 0 to the end at z = 1.",
          Tube(1.0, 2.0, 1.0, false), 0.25 * kPi * (4.0 - 1.0) * 1.0);

    // FV41: a free hollow cylinder. The wall is the thin one of the
    // sweep, where the ring formula corrected for axial Poisson inertia
    // is within 0.03%.
    Write(directory, "fv41", "NAFEMS FV41 -- free hollow cylinder (quarter sector)",
          "Mid-surface radius 1 m, wall 0.10 m, length 1 m, free at both ends. The breathing"
          " mode is the one that is radial and uniform along the axis.",
          Tube(0.95, 1.05, 1.0, true), 0.25 * kPi * (1.05 * 1.05 - 0.95 * 0.95) * 1.0);

    // FV42 IS NOT HERE, and the reason is worth stating rather than
    // leaving as a gap in the numbering.
    //
    // A hollow sphere is a revolve, and its profile -- the region between
    // two circles, in a half-plane -- necessarily *touches* the axis, at
    // both poles. The revolve refuses a profile that touches its axis by
    // name, so there is no feature tree for it and therefore no `.mepcad`
    // file. It is built instead from two `part.sphere` primitives and a
    // difference, in examples/nafems/fv42.lua, which writes a STEP file
    // the CAD pane can open -- a body without a history, which is exactly
    // what STEP is for.
    (void)&HollowSphere;

    // FV52: a simply-supported square plate, thick enough that thin-plate
    // theory is an upper bound rather than an answer.
    Write(directory, "fv52", "NAFEMS FV52 -- simply supported square plate",
          "10 x 10 x 1 m, simply supported along all four lower edges.",
          Prism({Vec2d{0, 0}, Vec2d{10, 0}, Vec2d{10, 10}, Vec2d{0, 10}}, 1.0, false), 100.0);

    // FV5: a deep simply-supported beam, where Timoshenko is the
    // reference and Euler-Bernoulli is visibly not.
    Write(directory, "fv5", "NAFEMS FV5 -- deep simply supported beam",
          "10 m span, 1 x 1 m square section, simply supported at both lower ends.",
          Prism({Vec2d{0, 0}, Vec2d{10, 0}, Vec2d{10, 1}, Vec2d{0, 1}}, 1.0, false), 10.0);

    // FV22: a clamped rhombic plate. All four side faces are built in,
    // which is the one support condition on this surface that needs no
    // edge at all.
    {
        const double skew = 45.0 * kPi / 180.0;
        const double run = 10.0 * std::cos(skew);
        const double rise = 10.0 * std::sin(skew);
        Write(directory, "fv22", "NAFEMS FV22 -- clamped rhombic plate",
              "10 m sides skewed 45 degrees, 1 m thick, clamped on all four side faces.",
              Prism({Vec2d{0, 0}, Vec2d{10, 0}, Vec2d{10 + run, rise}, Vec2d{run, rise}}, 1.0,
                    false),
              10.0 * rise * 1.0);
    }

    // FV32: a cantilevered tapered strip, in-plane vibration. A slab
    // again, and this one keeps its out-of-plane modes rather than
    // suppressing them -- suppressing them would make it plane strain.
    Write(directory, "fv32", "NAFEMS FV32 -- cantilevered tapered membrane",
          "10 m long, 2 m deep at the root and 0.5 m at the tip, 0.1 m thick, clamped at the"
          " root. The in-plane modes are picked out of the list by what they are.",
          Prism({Vec2d{0, -1.0}, Vec2d{10, -0.25}, Vec2d{10, 0.25}, Vec2d{0, 1.0}}, 0.1, true),
          0.5 * (2.0 + 0.5) * 10.0 * 0.1);

    if (failures != 0) {
        std::printf("%d of the example geometries is wrong\n", failures);
        return 1;
    }
    std::printf("every example geometry rebuilt and measured\n");
    return 0;
}
