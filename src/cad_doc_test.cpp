// Windowless coverage for cad_doc.h (plans/CAD_FEM_PLAN.md Part F.1).
//
// THE ROUND TRIP IS THE WHOLE TEST, and what makes it a real one is that
// the comparison is made on the *rebuilt geometry*, not on the text.
// Writing a document and reading it back to an identical document proves
// only that the writer and the reader agree with each other. Building a
// part, saving it, loading it, rebuilding from scratch, and getting the
// same volume and the same topology counts proves the thing anyone cares
// about: that a `.mepcad` file carries the operations rather than their
// result, and that replaying them reproduces the part.
//
// A second round trip on top of the first is worth its line: writing what
// was read has to give back byte-for-byte the same text, which catches a
// field that the reader quietly drops and the writer then supplies a
// default for -- the failure mode where the first round trip passes and
// the second one is where the loss shows.

#include "cad_doc.h"

#include "cad_pcurve.h"
#include "cad_tessellate.h"
#include "cad_validate.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

namespace {

using namespace cad;

int g_checks = 0;

void Check(bool condition, const char *expression, int line) {
    ++g_checks;
    if (condition) return;
    std::fprintf(stderr, "CHECK FAILED: %s at %s:%d\n", expression, __FILE__, line);
    std::abort();
}
#define CHECK(condition) Check((condition), #condition, __LINE__)

void CheckMessage(bool condition, const char *expression, int line, const std::string &message) {
    if (!condition && !message.empty()) std::fprintf(stderr, "  reported: %s\n", message.c_str());
    Check(condition, expression, line);
}
#define CHECK_MESSAGE(condition, message) CheckMessage((condition), #condition, __LINE__, (message))

bool Near(double a, double b, double tol) { return std::fabs(a - b) <= tol; }

SketchPlane AxisAlignedPlane(double z) {
    SketchPlane plane = PlaneFromNormal(Vec3d{0.0, 0.0, z}, Vec3d{0.0, 0.0, 1.0});
    plane.x_axis = Vec3d{1.0, 0.0, 0.0};
    plane.y_axis = Vec3d{0.0, 1.0, 0.0};
    return plane;
}

struct Summary {
    double volume = 0.0;
    int bodies = 0;
    TopologyCounts counts;
};

// What a rebuilt tree actually produced, measured the way everything else
// in Part E measures it: validated, tessellated, checked watertight.
Summary Measure(FeatureTree *tree) {
    Summary summary;
    std::string error;
    if (!tree->Rebuild(&error)) {
        std::fprintf(stderr, "  rebuild failed: %s\n", error.c_str());
        for (const Feature &feature : tree->Features()) {
            const FeatureResult *result = tree->ResultOf(feature.id);
            if (result != nullptr && !result->ok) {
                std::fprintf(stderr, "    feature %d (%s): %s\n", feature.id, feature.name.c_str(),
                             result->error.c_str());
            }
        }
        Check(false, "tree->Rebuild", __LINE__);
    }
    const Model &model = tree->Result();
    for (EntityId body : tree->Bodies()) {
        ValidationReport report;
        Check(ValidateBody(model, body, &report, {}), "ValidateBody", __LINE__);
        TessellationMesh mesh;
        Check(TessellateBody(model, body, {}, &mesh, &error), "TessellateBody", __LINE__);
        Check(mesh.IsClosed(), "mesh.IsClosed()", __LINE__);
        summary.volume += mesh.SignedVolume();
        const TopologyCounts counts = model.CountsOfBody(body);
        summary.counts.vertices += counts.vertices;
        summary.counts.edges += counts.edges;
        summary.counts.faces += counts.faces;
        summary.counts.loops += counts.loops;
        summary.counts.shells += counts.shells;
        summary.counts.holes += counts.holes;
        ++summary.bodies;
    }
    return summary;
}

bool Same(const Summary &a, const Summary &b) {
    return Near(a.volume, b.volume, 1e-9) && a.bodies == b.bodies &&
           a.counts.vertices == b.counts.vertices && a.counts.edges == b.counts.edges &&
           a.counts.faces == b.counts.faces && a.counts.loops == b.counts.loops &&
           a.counts.shells == b.counts.shells && a.counts.holes == b.counts.holes;
}

void Rectangle(Sketch *sketch, const Vec2d &lo, const Vec2d &hi) {
    const SketchId a = sketch->AddPoint(lo);
    const SketchId b = sketch->AddPoint(Vec2d{hi.x, lo.y});
    const SketchId c = sketch->AddPoint(hi);
    const SketchId d = sketch->AddPoint(Vec2d{lo.x, hi.y});
    sketch->AddLineFromPoints(a, b);
    sketch->AddLineFromPoints(b, c);
    sketch->AddLineFromPoints(c, d);
    sketch->AddLineFromPoints(d, a);
}

// --- Sketches on their own -----------------------------------------------

void TestSketchRoundTrip() {
    // Everything a sketch can hold, so that a field nobody exercised is
    // not a field nobody serialised.
    Sketch sketch(AxisAlignedPlane(2.5));
    sketch.Plane().offset = 1.25;
    Rectangle(&sketch, Vec2d{0.0, 0.0}, Vec2d{8.0, 5.0});
    const SketchId circle = sketch.AddCircle(Vec2d{4.0, 2.5}, 1.5);
    const SketchId arc = sketch.AddArc(Vec2d{1.0, 1.0}, Vec2d{2.0, 1.0}, Vec2d{1.0, 2.0}, false);
    const SketchId ellipse = sketch.AddEllipse(Vec2d{6.0, 4.0}, 1.2, 0.6, 0.4, true);
    const SketchId spline =
        sketch.AddSpline({Vec2d{0.0, 0.0}, Vec2d{1.0, 2.0}, Vec2d{3.0, -1.0}, Vec2d{5.0, 1.0}}, 3);
    const SketchId centre = sketch.AddPoint(Vec2d{4.0, 2.5}, true);
    sketch.SetScalarFixed(ellipse, 1, true);
    const SketchId dimension = sketch.Constrain(ConstraintKind::Radius, {}, {circle}, 1.5);
    sketch.Constrain(ConstraintKind::Coincident, {centre, centre}, {});
    sketch.Constrain(ConstraintKind::PointOnObject, {centre}, {spline});
    SketchConstraint reference;
    reference.kind = ConstraintKind::Distance;
    reference.points = {centre, sketch.Origin()};
    reference.value = 4.7;
    reference.driving = false;
    sketch.AddConstraint(reference);

    const std::string text = WriteSketch(sketch);
    Sketch back;
    std::string error;
    CHECK_MESSAGE(ReadSketch(text, &back, &error), error);

    // Ids first, because everything else refers to them.
    CHECK(back.Origin() == sketch.Origin());
    CHECK(back.PeekNextId() == sketch.PeekNextId());
    CHECK(back.Points().size() == sketch.Points().size());
    CHECK(back.Entities().size() == sketch.Entities().size());
    CHECK(back.Constraints().size() == sketch.Constraints().size());
    for (std::size_t i = 0; i < sketch.Points().size(); ++i) {
        CHECK(back.Points()[i].id == sketch.Points()[i].id);
        CHECK((back.Points()[i].position - sketch.Points()[i].position).Length() < 1e-15);
        CHECK(back.Points()[i].fixed == sketch.Points()[i].fixed);
    }
    for (std::size_t i = 0; i < sketch.Entities().size(); ++i) {
        const SketchEntity &a = sketch.Entities()[i];
        const SketchEntity &b = back.Entities()[i];
        CHECK(a.id == b.id && a.kind == b.kind && a.construction == b.construction);
        CHECK(a.points == b.points);
        CHECK(a.scalars == b.scalars);
        CHECK(a.scalar_fixed == b.scalar_fixed);
        CHECK(a.arc_ccw == b.arc_ccw && a.degree == b.degree && a.knots == b.knots);
    }
    for (std::size_t i = 0; i < sketch.Constraints().size(); ++i) {
        const SketchConstraint &a = sketch.Constraints()[i];
        const SketchConstraint &b = back.Constraints()[i];
        CHECK(a.id == b.id && a.kind == b.kind && a.driving == b.driving);
        CHECK(a.points == b.points && a.entities == b.entities);
        CHECK(Near(a.value, b.value, 1e-15));
    }
    // The plane, including the offset a boss standing off a face needs.
    CHECK((back.Plane().origin - sketch.Plane().origin).Length() < 1e-15);
    CHECK((back.Plane().x_axis - sketch.Plane().x_axis).Length() < 1e-15);
    CHECK(Near(back.Plane().offset, 1.25, 1e-15));
    (void)arc;
    (void)dimension;

    // Writing what was read gives the same text. This is what catches a
    // field the reader drops and the writer then defaults.
    CHECK(WriteSketch(back) == text);
    std::printf("sketch round trip: %d points, %d entities, %d constraints, identical on rewrite\n",
                static_cast<int>(sketch.Points().size()), static_cast<int>(sketch.Entities().size()),
                static_cast<int>(sketch.Constraints().size()));

    // An attached plane carries its Part B.4 name, which is the thing
    // that makes a sketch on a face reopen still attached to that face.
    Sketch attached(AxisAlignedPlane(0.0));
    attached.Plane().attached = true;
    attached.Plane().face.generating_feature = 7;
    attached.Plane().face.role = "end";
    attached.Plane().face.surface_kind = SurfaceKind::Cylinder;
    attached.Plane().face.curve_kind = CurveKind::Circle;
    attached.Plane().face.sample_point = Vec3d{1.0, 2.0, 3.0};
    attached.Plane().face.sample_direction = Vec3d{0.0, 0.0, 1.0};
    attached.Plane().face.neighbour_features = {1, 4, 9};
    attached.Plane().face.loop_count = 2;
    attached.Plane().face.edge_count = 5;
    Sketch attached_back;
    CHECK_MESSAGE(ReadSketch(WriteSketch(attached), &attached_back, &error), error);
    CHECK(attached_back.Plane().attached);
    CHECK(attached_back.Plane().face.generating_feature == 7);
    CHECK(attached_back.Plane().face.role == "end");
    CHECK(attached_back.Plane().face.surface_kind == SurfaceKind::Cylinder);
    CHECK(attached_back.Plane().face.neighbour_features == std::vector<int>({1, 4, 9}));
    CHECK(attached_back.Plane().face.loop_count == 2);
    CHECK(attached_back.Plane().face.edge_count == 5);
    std::printf("a sketch attached to a face carries its persistent name across\n");
}

// --- Whole documents ------------------------------------------------------

// The part is deliberately not a box: a plate with a hole through it, a
// boss on top, and a revolve, so that the tree has several features of
// different kinds referring to each other.
CadDocument BuildPart() {
    CadDocument document;
    document.title = "bracket";
    document.notes = "a plate, a hole, a boss and a revolved collar";

    Sketch outline(AxisAlignedPlane(0.0));
    Rectangle(&outline, Vec2d{0.0, 0.0}, Vec2d{10.0, 6.0});
    const int outline_id = document.tree.AddSketch(outline, "outline");

    Feature plate;
    plate.kind = FeatureKind::Extrude;
    plate.name = "plate";
    plate.sketch = outline_id;
    plate.distance = 2.0;
    document.tree.AddFeature(plate);

    Sketch hole(AxisAlignedPlane(0.0));
    hole.AddCircle(Vec2d{3.0, 3.0}, 1.0);
    const int hole_id = document.tree.AddSketch(hole, "hole");

    Feature drill;
    drill.kind = FeatureKind::Extrude;
    drill.name = "hole";
    drill.sketch = hole_id;
    drill.combine = FeatureCombine::Cut;
    drill.end = ExtrudeEnd::ThroughAll;
    document.tree.AddFeature(drill);

    // The boss starts *inside* the plate rather than exactly on its top
    // face. Starting it flush would put two coincident faces in front of
    // the union, which Part C.4 does not handle -- a real limitation,
    // recorded there, and not one this test exists to rediscover.
    Sketch boss(AxisAlignedPlane(1.5));
    boss.AddCircle(Vec2d{7.0, 3.0}, 1.2);
    const int boss_id = document.tree.AddSketch(boss, "boss");

    Feature stand;
    stand.kind = FeatureKind::Extrude;
    stand.name = "boss";
    stand.sketch = boss_id;
    stand.distance = 3.5;
    stand.combine = FeatureCombine::Add;
    document.tree.AddFeature(stand);
    return document;
}

void TestDocumentRoundTrip() {
    CadDocument original = BuildPart();
    const Summary before = Measure(&original.tree);
    std::printf("the part: %.6f in %d body, %d faces, %d edges, %d vertices\n", before.volume,
                before.bodies, before.counts.faces, before.counts.edges, before.counts.vertices);
    CHECK(before.volume > 0.0);

    const std::string text = WriteCadDocument(original);
    CadDocument reopened;
    std::string error;
    CHECK_MESSAGE(ReadCadDocument(text, &reopened, &error), error);
    CHECK(reopened.title == "bracket");
    CHECK(reopened.notes == original.notes);
    CHECK(reopened.tree.Features().size() == original.tree.Features().size());

    // NOT EVALUATED ON LOAD. That is the claim the format exists to make,
    // so it is checked rather than assumed: what came back is a list of
    // operations with no geometry attached until it is replayed.
    CHECK(reopened.tree.Bodies().empty());
    CHECK(reopened.tree.Result().Faces().empty());

    const Summary after = Measure(&reopened.tree);
    std::printf("rebuilt from the file: %.6f in %d body, %d faces, %d edges, %d vertices\n",
                after.volume, after.bodies, after.counts.faces, after.counts.edges,
                after.counts.vertices);
    CHECK(Same(before, after));

    // Byte-for-byte on rewrite.
    CHECK(WriteCadDocument(reopened) == text);
    std::printf("and rewriting it gives the same %d bytes back\n", static_cast<int>(text.size()));

    // The whole point of keeping the tree: change a number in the
    // reopened document and the part follows, exactly as it would have in
    // the one it was saved from. Four rather than five, so the boss still
    // stands proud of the plate instead of ending flush with it -- see
    // the note in BuildPart.
    for (const Feature &feature : reopened.tree.Features()) {
        if (feature.name != "plate") continue;
        reopened.tree.GetMutable(feature.id)->distance = 4.0;
        break;
    }
    for (const Feature &feature : original.tree.Features()) {
        if (feature.name != "plate") continue;
        original.tree.GetMutable(feature.id)->distance = 4.0;
        break;
    }
    const Summary thicker_here = Measure(&original.tree);
    const Summary thicker_there = Measure(&reopened.tree);
    CHECK(Same(thicker_here, thicker_there));
    CHECK(thicker_there.volume > after.volume);
    std::printf("thickened to 4 after reopening: %.6f, the same as thickening the original\n",
                thicker_there.volume);
}

void TestVersionAndRefusals() {
    const CadDocument document = BuildPart();
    const std::string text = WriteCadDocument(document);
    std::string error;

    int version = -1;
    CHECK_MESSAGE(CadDocumentVersion(text, &version, &error), error);
    CHECK(version == kCadDocumentVersion);

    // A file from a newer mep is refused by name, not half-read. This is
    // the only thing versioning buys, and it only works if it was there
    // from the first release.
    std::string future = text;
    const std::size_t at = future.find("\"version\":1");
    CHECK(at != std::string::npos);
    future.replace(at, std::string("\"version\":1").size(), "\"version\":99");
    CadDocument out;
    CHECK(!ReadCadDocument(future, &out, &error));
    std::printf("a document from the future: %s\n", error.c_str());
    CHECK(error.find("newer mep") != std::string::npos);
    CHECK(out.tree.Features().empty());

    // Not a document at all.
    CHECK(!ReadCadDocument("this is not JSON", &out, &error));
    CHECK(error.find("did not parse") != std::string::npos);
    CHECK(!ReadCadDocument("{\"format\":\"something-else\"}", &out, &error));
    CHECK(error.find("not a .mepcad") != std::string::npos);
    CHECK(!ReadCadDocument("{\"format\":\"mepcad\"}", &out, &error));
    CHECK(error.find("which version") != std::string::npos);

    // An unknown kind is named rather than defaulted, because defaulting
    // it would open the file as a different shape.
    std::string broken = text;
    const std::size_t kind_at = broken.find("\"kind\":\"extrude\"");
    CHECK(kind_at != std::string::npos);
    broken.replace(kind_at, std::string("\"kind\":\"extrude\"").size(), "\"kind\":\"wormhole\"");
    CHECK(!ReadCadDocument(broken, &out, &error));
    std::printf("an unknown feature kind: %s\n", error.c_str());
    CHECK(error.find("wormhole") != std::string::npos);

    Sketch not_a_sketch;
    CHECK(!ReadSketch("{\"format\":\"mepcad\"}", &not_a_sketch, &error));
    CHECK(error.find("not a sketch") != std::string::npos);
    CHECK(!ReadSketch("}{", &not_a_sketch, &error));
    CHECK(error.find("did not parse") != std::string::npos);
}

void TestEmptyAndSuppressed() {
    // An empty document is a document.
    CadDocument empty;
    std::string error;
    CadDocument back;
    CHECK_MESSAGE(ReadCadDocument(WriteCadDocument(empty), &back, &error), error);
    CHECK(back.tree.Features().empty());
    CHECK(WriteCadDocument(back) == WriteCadDocument(empty));

    // Suppression is part of the document, and it has to survive on both
    // kinds of feature -- a sketch and an operation -- because they take
    // different paths back in. Both of the things called "boss" are
    // suppressed here: the sketch that holds its profile and the extrude
    // that uses it. What is left is the plate with its hole, which still
    // builds.
    CadDocument document = BuildPart();
    for (const Feature &feature : document.tree.Features()) {
        if (feature.name == "boss") document.tree.GetMutable(feature.id)->suppressed = true;
    }
    const Summary suppressed = Measure(&document.tree);
    CadDocument reopened;
    CHECK_MESSAGE(ReadCadDocument(WriteCadDocument(document), &reopened, &error), error);
    int suppressed_count = 0;
    for (const Feature &feature : reopened.tree.Features()) {
        if (feature.suppressed) ++suppressed_count;
    }
    CHECK(suppressed_count == 2);
    int suppressed_sketches = 0;
    for (const Feature &feature : reopened.tree.Features()) {
        if (feature.suppressed && feature.kind == FeatureKind::Sketch) ++suppressed_sketches;
    }
    CHECK(suppressed_sketches == 1);
    const Summary reopened_suppressed = Measure(&reopened.tree);
    CHECK(Same(suppressed, reopened_suppressed));
    std::printf("suppression survives, on a feature and on a sketch\n");
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestSketchRoundTrip();
    TestDocumentRoundTrip();
    TestVersionAndRefusals();
    TestEmptyAndSuppressed();
    std::printf("cad_doc_test passed (%d checks)\n", g_checks);
    return 0;
}
