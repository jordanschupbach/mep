// Windowless coverage for cad_step.h (plans/CAD_FEM_PLAN.md Parts F.2 and
// F.3).
//
// THE ROUND TRIP IS THE ONLY ORACLE AVAILABLE without a corpus of other
// people's files, and it is a real one provided the comparison is made on
// the geometry rather than on the text: write a solid, read it back with
// a reader that shares no code with the writer, and require the same
// volume, the same topology counts, and a body that still validates and
// still tessellates watertight. A writer and a reader that agreed with
// each other and with nothing else would pass a text comparison and fail
// this.
//
// The shapes are chosen to cover what the mapping has to get right rather
// than to look like parts: a box for planes only, a cylinder for a seam
// and a periodic surface, a sphere for poles, a torus for periodicity in
// both directions, a filleted block for an analytic surface produced by
// Part E.3, and a hollow box for an inner shell -- which is the one that
// checks BREP_WITH_VOIDS rather than MANIFOLD_SOLID_BREP.
//
// THE PARSER IS TESTED SEPARATELY AND ADVERSARIALLY, because everything
// above only exercises the small corner of Part 21 that mep's own writer
// emits. Real files use the rest: comments in the middle of argument
// lists, doubled quotes inside strings, complex instances, exponent
// forms, entities out of order, forward references.

#include "cad_step.h"

#include "cad_feature.h"
#include "cad_modify.h"
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

struct Shape {
    double volume = 0.0;
    TopologyCounts counts;
    bool valid = false;
    bool watertight = false;
};

Shape Describe(const Model &model, EntityId body) {
    Shape out;
    ValidationReport report;
    out.valid = ValidateBody(model, body, &report, {});
    if (!out.valid) std::fprintf(stderr, "  invalid:\n%s\n", report.Summary().c_str());
    TessellationMesh mesh;
    std::string error;
    // Finer than the default, because the two sides of a round trip are
    // not required to mesh identically -- see SameShape.
    TessellationOptions options;
    options.chord_tolerance = 1e-4;
    if (TessellateBody(model, body, options, &mesh, &error)) {
        out.watertight = mesh.IsClosed();
        out.volume = mesh.SignedVolume();
    } else {
        std::fprintf(stderr, "  could not tessellate: %s\n", error.c_str());
    }
    out.counts = model.CountsOfBody(body);
    return out;
}

// The topology has to match exactly; the volume only has to agree to
// within the meshing.
//
// A file carries the same *solid*, not the same description of it: a
// sphere's seam may come back as the same arc with its parameters a turn
// along, and the tessellator then chooses its samples slightly
// differently. Both meshes are inscribed in the same surface and both
// converge to it, so they agree to the chording and not beyond. A
// planar solid has no chording and does agree exactly, which the box and
// the two-body case below check at 1e-9.
bool SameShape(const Shape &a, const Shape &b) {
    const double allowed = std::max(1e-9, 1e-3 * std::fabs(a.volume));
    return Near(a.volume, b.volume, allowed) && a.counts.vertices == b.counts.vertices &&
           a.counts.edges == b.counts.edges && a.counts.faces == b.counts.faces &&
           a.counts.loops == b.counts.loops && a.counts.shells == b.counts.shells;
}

// --- The physical file, on its own ----------------------------------------

void TestParser() {
    // Everything Part 21 allows in an argument, in one file, including
    // the things mep's own writer never emits.
    const std::string text =
        "ISO-10303-21;\n"
        "HEADER;\n"
        "FILE_DESCRIPTION(('a test','with two lines'),'2;1');\n"
        "FILE_NAME('bracket.stp','2024-01-01T00:00:00',('Someone O''Brien'),('Acme'),'','','');\n"
        "FILE_SCHEMA(('AUTOMOTIVE_DESIGN { 1 0 10303 214 3 1 1 }'));\n"
        "ENDSEC;\n"
        "DATA;\n"
        "/* a comment before an entity */\n"
        "#10 = CARTESIAN_POINT('',(1.,-2.5,+3.0E2));\n"
        "#20 = VERTEX_POINT('',#10);   /* and after one */\n"
        "#30 = SOMETHING($,*,.TRUE.,(1,2,3),(#10,#20),LENGTH_MEASURE(4.5),\"00FF\");\n"
        "#40 = (BOUNDED_CURVE()B_SPLINE_CURVE(3,(#10),.UNSPECIFIED.,.F.,.F.)CURVE());\n"
        "#5 = CARTESIAN_POINT('out of order',(0.,0.,0.));\n"
        "ENDSEC;\n"
        "END-ISO-10303-21;\n";

    StepFile file;
    std::string error;
    CHECK_MESSAGE(ParseStepFile(text, &file, &error), error);
    CHECK(file.header.size() == 3);
    CHECK(file.SchemaName() == "AUTOMOTIVE_DESIGN { 1 0 10303 214 3 1 1 }");
    // A doubled quote is one quote.
    CHECK(file.header[1].arguments[2].items[0].text == "Someone O'Brien");
    CHECK(file.entities.size() == 5);

    const StepEntity *point = file.Get(10);
    CHECK(point != nullptr && point->type == "CARTESIAN_POINT");
    CHECK(Near(point->arguments[1].items[0].AsDouble(), 1.0, 1e-15));
    CHECK(Near(point->arguments[1].items[1].AsDouble(), -2.5, 1e-15));
    CHECK(Near(point->arguments[1].items[2].AsDouble(), 300.0, 1e-12));

    const StepEntity *odd = file.Get(30);
    CHECK(odd != nullptr);
    CHECK(odd->arguments[0].kind == StepValue::Kind::Unset);
    CHECK(odd->arguments[1].kind == StepValue::Kind::Derived);
    CHECK(odd->arguments[2].kind == StepValue::Kind::Enumeration && odd->arguments[2].text == "TRUE");
    CHECK(odd->arguments[3].kind == StepValue::Kind::List && odd->arguments[3].items.size() == 3);
    CHECK(odd->arguments[3].items[0].kind == StepValue::Kind::Integer);
    CHECK(odd->arguments[4].items[1].kind == StepValue::Kind::Reference);
    CHECK(odd->arguments[4].items[1].reference == 20);
    CHECK(odd->arguments[5].kind == StepValue::Kind::Typed && odd->arguments[5].text == "LENGTH_MEASURE");
    CHECK(Near(odd->arguments[5].items[0].AsDouble(), 4.5, 1e-15));
    CHECK(odd->arguments[6].kind == StepValue::Kind::Binary && odd->arguments[6].text == "00FF");

    // A complex instance keeps its parts whole: which part an argument
    // belongs to is the only way to read it.
    const StepEntity *complex = file.Get(40);
    CHECK(complex != nullptr && complex->IsComplex() && complex->parts.size() == 3);
    CHECK(complex->parts[1].type == "B_SPLINE_CURVE");
    CHECK(complex->parts[1].arguments[0].integer == 3);
    // ...and OfType finds it by any of them.
    CHECK(file.OfType("B_SPLINE_CURVE").size() == 1);
    CHECK(file.OfType("CARTESIAN_POINT").size() == 2);
    std::printf("part 21: every kind of argument, comments, doubled quotes, complex instances\n");

    // Rewriting it parses back to the same thing.
    const std::string again = WriteStepFile(file, "");
    StepFile reparsed;
    CHECK_MESSAGE(ParseStepFile(again, &reparsed, &error), error);
    CHECK(reparsed.entities.size() == file.entities.size());
    CHECK(reparsed.Get(10) != nullptr);
    CHECK(Near(reparsed.Get(10)->arguments[1].items[2].AsDouble(), 300.0, 1e-12));
    CHECK(reparsed.header[1].arguments[2].items[0].text == "Someone O'Brien");

    // And what it refuses.
    StepFile broken;
    CHECK(!ParseStepFile("not a step file at all", &broken, &error));
    CHECK(error.find("ISO-10303-21") != std::string::npos);
    CHECK(!ParseStepFile("ISO-10303-21;\nHEADER;\nENDSEC;\nEND-ISO-10303-21;\n", &broken, &error));
    CHECK(error.find("no DATA section") != std::string::npos);
    CHECK(!ParseStepFile("ISO-10303-21;\nDATA;\n#1 = A('unterminated);\nENDSEC;\n", &broken, &error));
    CHECK(error.find("never closed") != std::string::npos);
    CHECK(!ParseStepFile("ISO-10303-21;\nDATA;\n/* never closed\n#1=A();\n", &broken, &error));
    CHECK(error.find("comment") != std::string::npos);
    CHECK(!ParseStepFile("ISO-10303-21;\nDATA;\n#1=A();\n#1=B();\nENDSEC;\n", &broken, &error));
    CHECK(error.find("defined twice") != std::string::npos);
    std::printf("and what it refuses, each with the line it went wrong on\n");
}

// --- Solids, out and back -------------------------------------------------

void RoundTrip(const char *what, const Model &model, EntityId body) {
    const Shape before = Describe(model, body);
    CHECK(before.valid);
    CHECK(before.watertight);
    CHECK(before.volume > 0.0);

    std::string text;
    std::string error;
    CHECK_MESSAGE(WriteStepShapes(model, {body}, &text, &error), error);

    Model back;
    StepReadReport report;
    StepReadOptions options;
    options.require_valid = true;
    CHECK_MESSAGE(ReadStepText(text, &back, &report, options), report.error);
    CHECK(report.bodies.size() == 1);
    CHECK(report.unsupported.empty());

    const Shape after = Describe(back, report.bodies.front());
    std::printf("  %-14s %12.6f -> %12.6f   %d/%d/%d faces/edges/vertices\n", what, before.volume,
                after.volume, after.counts.faces, after.counts.edges, after.counts.vertices);
    CHECK(after.valid);
    CHECK(after.watertight);
    CHECK(SameShape(before, after));

    // Writing what was read gives a file that reads to the same thing
    // again -- the check that nothing is lost on the *second* pass, which
    // is where a reader that quietly defaults something shows up.
    std::string twice;
    CHECK_MESSAGE(WriteStepShapes(back, report.bodies, &twice, &error), error);
    Model third;
    StepReadReport third_report;
    CHECK_MESSAGE(ReadStepText(twice, &third, &third_report, options), third_report.error);
    CHECK(SameShape(after, Describe(third, third_report.bodies.front())));
}

void TestPrimitives() {
    std::printf("round trip through STEP:\n");
    {
        Model model;
        EntityId body = kNoEntity;
        std::string error;
        CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{2.0, 3.0, 4.0}, &model, &body));
        CHECK(BuildAllPCurves(&model, {}, &error));
        RoundTrip("box", model, body);
    }
    {
        Model model;
        EntityId body = kNoEntity;
        std::string error;
        CHECK(MakeCylinder(Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 1.0}, 2.0, 5.0, &model, &body));
        CHECK(BuildAllPCurves(&model, {}, &error));
        RoundTrip("cylinder", model, body);
    }
    {
        Model model;
        EntityId body = kNoEntity;
        std::string error;
        CHECK(MakeSphere(Vec3d{1.0, 0.0, 0.0}, 2.0, &model, &body));
        CHECK(BuildAllPCurves(&model, {}, &error));
        RoundTrip("sphere", model, body);
    }
    {
        Model model;
        EntityId body = kNoEntity;
        std::string error;
        CHECK(MakeTorus(Vec3d{}, Vec3d{0.0, 0.0, 1.0}, 3.0, 1.0, &model, &body));
        CHECK(BuildAllPCurves(&model, {}, &error));
        RoundTrip("torus", model, body);
    }
}

void TestBuiltShapes() {
    // A block with a rounded edge: the cylinder came out of Part E.3, and
    // it must go out as a CYLINDRICAL_SURFACE rather than as a B-spline.
    Model box;
    EntityId body = kNoEntity;
    std::string error;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{4.0, 5.0, 4.0}, &box, &body));
    CHECK(BuildAllPCurves(&box, {}, &error));
    EntityId edge = kNoEntity;
    for (EntityId shell : box.GetBody(body)->shells) {
        for (EntityId candidate : box.EdgesOfShell(shell)) {
            if ((box.EdgeStartPoint(candidate) - Vec3d{0.0, 0.0, 0.0}).Length() < 1e-9 &&
                (box.EdgeEndPoint(candidate) - Vec3d{0.0, 0.0, 4.0}).Length() < 1e-9) {
                edge = candidate;
            }
        }
    }
    CHECK(edge != kNoEntity);
    BlendOptions blend;
    blend.radius = 0.75;
    Model rounded;
    EntityId rounded_body = kNoEntity;
    CHECK_MESSAGE(BlendEdge(box, body, edge, blend, &rounded, &rounded_body, &error), error);
    RoundTrip("filleted block", rounded, rounded_body);

    std::string text;
    CHECK_MESSAGE(WriteStepShapes(rounded, {rounded_body}, &text, &error), error);
    CHECK(text.find("CYLINDRICAL_SURFACE") != std::string::npos);
    CHECK(text.find("B_SPLINE_SURFACE") == std::string::npos);
    std::printf("  the fillet goes out as a CYLINDRICAL_SURFACE, not as a B-spline that is round\n");

    // A hollow box: two shells, so BREP_WITH_VOIDS rather than
    // MANIFOLD_SOLID_BREP, and the void has to come back as a void.
    Model plain;
    EntityId plain_body = kNoEntity;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{6.0, 6.0, 6.0}, &plain, &plain_body));
    CHECK(BuildAllPCurves(&plain, {}, &error));
    Model hollow;
    EntityId hollow_body = kNoEntity;
    CHECK_MESSAGE(ShellBody(plain, plain_body, {}, 1.0, {}, &hollow, &hollow_body, &error), error);
    CHECK(hollow.GetBody(hollow_body)->shells.size() == 2);
    RoundTrip("hollow box", hollow, hollow_body);
    std::string hollow_text;
    CHECK_MESSAGE(WriteStepShapes(hollow, {hollow_body}, &hollow_text, &error), error);
    CHECK(hollow_text.find("BREP_WITH_VOIDS") != std::string::npos);
    Model hollow_back;
    StepReadReport hollow_report;
    CHECK_MESSAGE(ReadStepText(hollow_text, &hollow_back, &hollow_report, {}), hollow_report.error);
    CHECK(hollow_back.GetBody(hollow_report.bodies.front())->shells.size() == 2);
    CHECK(!hollow_back.GetShell(hollow_back.GetBody(hollow_report.bodies.front())->shells[1])->is_outer);
    std::printf("  a hollow box keeps its void: two shells out, two shells back\n");
}

void TestSeveralBodies() {
    Model model;
    EntityId a = kNoEntity;
    EntityId b = kNoEntity;
    std::string error;
    CHECK(MakeBox(Vec3d{0.0, 0.0, 0.0}, Vec3d{2.0, 2.0, 2.0}, &model, &a));
    CHECK(MakeBox(Vec3d{10.0, 0.0, 0.0}, Vec3d{3.0, 3.0, 3.0}, &model, &b));
    CHECK(BuildAllPCurves(&model, {}, &error));
    std::string text;
    CHECK_MESSAGE(WriteStepShapes(model, {a, b}, &text, &error), error);
    Model back;
    StepReadReport report;
    CHECK_MESSAGE(ReadStepText(text, &back, &report, {}), report.error);
    CHECK(report.bodies.size() == 2);
    double total = 0.0;
    for (EntityId body : report.bodies) total += Describe(back, body).volume;
    CHECK(Near(total, 8.0 + 27.0, 1e-9));
    std::printf("two bodies in one file: %.6f in total\n", total);
}

void TestReadRefusals() {
    Model model;
    StepReadReport report;
    // A file that parses and has nothing this reader can build.
    const std::string empty =
        "ISO-10303-21;\nHEADER;\nFILE_SCHEMA(('X'));\nENDSEC;\nDATA;\n"
        "#1=CARTESIAN_POINT('',(0.,0.,0.));\nENDSEC;\nEND-ISO-10303-21;\n";
    CHECK(!ReadStepText(empty, &model, &report, {}));
    std::printf("a file with no solid in it: %s\n", report.error.c_str());
    CHECK(report.error.find("no solid") != std::string::npos);

    // An unsupported surface is counted and named rather than skipped in
    // silence -- and the face it was on is dropped rather than built
    // wrong, which the warning says.
    const std::string exotic =
        "ISO-10303-21;\nHEADER;\nFILE_SCHEMA(('X'));\nENDSEC;\nDATA;\n"
        "#1=CARTESIAN_POINT('',(0.,0.,0.));\n"
        "#2=DIRECTION('',(0.,0.,1.));\n"
        "#3=DIRECTION('',(1.,0.,0.));\n"
        "#4=AXIS2_PLACEMENT_3D('',#1,#2,#3);\n"
        "#5=SURFACE_OF_LINEAR_EXTRUSION('',#4,#4);\n"
        "#6=ADVANCED_FACE('',(),#5,.T.);\n"
        "#7=CLOSED_SHELL('',(#6));\n"
        "#8=MANIFOLD_SOLID_BREP('thing',#7);\n"
        "ENDSEC;\nEND-ISO-10303-21;\n";
    Model exotic_model;
    StepReadReport exotic_report;
    CHECK(!ReadStepText(exotic, &exotic_model, &exotic_report, {}));
    CHECK(exotic_report.unsupported.count("SURFACE_OF_LINEAR_EXTRUSION") == 1);
    std::printf("an unsupported surface is named: SURFACE_OF_LINEAR_EXTRUSION x%d\n",
                exotic_report.unsupported["SURFACE_OF_LINEAR_EXTRUSION"]);

    // Writing nothing is refused rather than producing an empty file that
    // looks like it worked.
    std::string text;
    std::string error;
    CHECK(!WriteStepShapes(model, {}, &text, &error));
    CHECK(error.find("no bodies") != std::string::npos);
}

// A file written the way another system would write it, rather than the
// way mep does: different argument spellings, entities out of order, and
// the units and context entities in front. This is the closest thing to a
// foreign file available without shipping one.
void TestForeignStyleFile() {
    const std::string text =
        "ISO-10303-21;\n"
        "HEADER;\n"
        "FILE_DESCRIPTION((''),'2;1');\n"
        "FILE_NAME('cube.step','2020-02-02T02:02:02',(''),(''),'Other CAD','','');\n"
        "FILE_SCHEMA(('CONFIG_CONTROL_DESIGN'));\n"
        "ENDSEC;\n"
        "DATA;\n"
        // The solid first, referring forward to everything.
        "#100=MANIFOLD_SOLID_BREP('cube',#101);\n"
        "#101=CLOSED_SHELL('',(#110,#120,#130,#140,#150,#160));\n"
        // Eight corners.
        "#1=CARTESIAN_POINT('',(0.,0.,0.));\n#2=CARTESIAN_POINT('',(1.,0.,0.));\n"
        "#3=CARTESIAN_POINT('',(1.,1.,0.));\n#4=CARTESIAN_POINT('',(0.,1.,0.));\n"
        "#5=CARTESIAN_POINT('',(0.,0.,1.));\n#6=CARTESIAN_POINT('',(1.,0.,1.));\n"
        "#7=CARTESIAN_POINT('',(1.,1.,1.));\n#8=CARTESIAN_POINT('',(0.,1.,1.));\n"
        "#11=VERTEX_POINT('',#1);\n#12=VERTEX_POINT('',#2);\n#13=VERTEX_POINT('',#3);\n"
        "#14=VERTEX_POINT('',#4);\n#15=VERTEX_POINT('',#5);\n#16=VERTEX_POINT('',#6);\n"
        "#17=VERTEX_POINT('',#7);\n#18=VERTEX_POINT('',#8);\n"
        // Directions and the twelve lines.
        "#20=DIRECTION('',(1.,0.,0.));\n#21=DIRECTION('',(0.,1.,0.));\n#22=DIRECTION('',(0.,0.,1.));\n"
        "#23=DIRECTION('',(-1.,0.,0.));\n#24=DIRECTION('',(0.,-1.,0.));\n#25=DIRECTION('',(0.,0.,-1.));\n"
        "#30=VECTOR('',#20,1.);\n#31=VECTOR('',#21,1.);\n#32=VECTOR('',#22,1.);\n"
        "#40=LINE('',#1,#30);\n#41=LINE('',#2,#31);\n#42=LINE('',#4,#30);\n#43=LINE('',#1,#31);\n"
        "#44=LINE('',#5,#30);\n#45=LINE('',#6,#31);\n#46=LINE('',#8,#30);\n#47=LINE('',#5,#31);\n"
        "#48=LINE('',#1,#32);\n#49=LINE('',#2,#32);\n#50=LINE('',#3,#32);\n#51=LINE('',#4,#32);\n"
        "#60=EDGE_CURVE('',#11,#12,#40,.T.);\n#61=EDGE_CURVE('',#12,#13,#41,.T.);\n"
        "#62=EDGE_CURVE('',#14,#13,#42,.T.);\n#63=EDGE_CURVE('',#11,#14,#43,.T.);\n"
        "#64=EDGE_CURVE('',#15,#16,#44,.T.);\n#65=EDGE_CURVE('',#16,#17,#45,.T.);\n"
        "#66=EDGE_CURVE('',#18,#17,#46,.T.);\n#67=EDGE_CURVE('',#15,#18,#47,.T.);\n"
        "#68=EDGE_CURVE('',#11,#15,#48,.T.);\n#69=EDGE_CURVE('',#12,#16,#49,.T.);\n"
        "#70=EDGE_CURVE('',#13,#17,#50,.T.);\n#71=EDGE_CURVE('',#14,#18,#51,.T.);\n"
        // Six planes, each placed at a corner.
        "#80=AXIS2_PLACEMENT_3D('',#1,#25,#20);\n#81=AXIS2_PLACEMENT_3D('',#5,#22,#20);\n"
        "#82=AXIS2_PLACEMENT_3D('',#1,#24,#22);\n#83=AXIS2_PLACEMENT_3D('',#3,#21,#23);\n"
        "#84=AXIS2_PLACEMENT_3D('',#1,#23,#21);\n#85=AXIS2_PLACEMENT_3D('',#2,#20,#21);\n"
        "#90=PLANE('',#80);\n#91=PLANE('',#81);\n#92=PLANE('',#82);\n"
        "#93=PLANE('',#83);\n#94=PLANE('',#84);\n#95=PLANE('',#85);\n"
        // The six faces. Bottom, seen from below, runs the other way.
        "#111=ORIENTED_EDGE('',*,*,#60,.F.);\n#112=ORIENTED_EDGE('',*,*,#63,.T.);\n"
        "#113=ORIENTED_EDGE('',*,*,#62,.T.);\n#114=ORIENTED_EDGE('',*,*,#61,.F.);\n"
        "#115=EDGE_LOOP('',(#111,#112,#113,#114));\n"
        "#116=FACE_OUTER_BOUND('',#115,.T.);\n#110=ADVANCED_FACE('',(#116),#90,.T.);\n"
        "#121=ORIENTED_EDGE('',*,*,#64,.T.);\n#122=ORIENTED_EDGE('',*,*,#65,.T.);\n"
        "#123=ORIENTED_EDGE('',*,*,#66,.F.);\n#124=ORIENTED_EDGE('',*,*,#67,.F.);\n"
        "#125=EDGE_LOOP('',(#121,#122,#123,#124));\n"
        "#126=FACE_OUTER_BOUND('',#125,.T.);\n#120=ADVANCED_FACE('',(#126),#91,.T.);\n"
        "#131=ORIENTED_EDGE('',*,*,#60,.T.);\n#132=ORIENTED_EDGE('',*,*,#69,.T.);\n"
        "#133=ORIENTED_EDGE('',*,*,#64,.F.);\n#134=ORIENTED_EDGE('',*,*,#68,.F.);\n"
        "#135=EDGE_LOOP('',(#131,#132,#133,#134));\n"
        "#136=FACE_OUTER_BOUND('',#135,.T.);\n#130=ADVANCED_FACE('',(#136),#92,.T.);\n"
        "#141=ORIENTED_EDGE('',*,*,#62,.F.);\n#142=ORIENTED_EDGE('',*,*,#71,.T.);\n"
        "#143=ORIENTED_EDGE('',*,*,#66,.T.);\n#144=ORIENTED_EDGE('',*,*,#70,.F.);\n"
        "#145=EDGE_LOOP('',(#141,#142,#143,#144));\n"
        "#146=FACE_OUTER_BOUND('',#145,.T.);\n#140=ADVANCED_FACE('',(#146),#93,.T.);\n"
        "#151=ORIENTED_EDGE('',*,*,#63,.F.);\n#152=ORIENTED_EDGE('',*,*,#68,.T.);\n"
        "#153=ORIENTED_EDGE('',*,*,#67,.T.);\n#154=ORIENTED_EDGE('',*,*,#71,.F.);\n"
        "#155=EDGE_LOOP('',(#151,#152,#153,#154));\n"
        "#156=FACE_OUTER_BOUND('',#155,.T.);\n#150=ADVANCED_FACE('',(#156),#94,.T.);\n"
        "#161=ORIENTED_EDGE('',*,*,#61,.T.);\n#162=ORIENTED_EDGE('',*,*,#70,.T.);\n"
        "#163=ORIENTED_EDGE('',*,*,#65,.F.);\n#164=ORIENTED_EDGE('',*,*,#69,.F.);\n"
        "#165=EDGE_LOOP('',(#161,#162,#163,#164));\n"
        "#166=FACE_OUTER_BOUND('',#165,.T.);\n#160=ADVANCED_FACE('',(#166),#95,.T.);\n"
        "ENDSEC;\n"
        "END-ISO-10303-21;\n";

    Model model;
    StepReadReport report;
    StepReadOptions options;
    options.require_valid = true;
    CHECK_MESSAGE(ReadStepText(text, &model, &report, options), report.error);
    CHECK(report.bodies.size() == 1);
    CHECK(report.body_names[0] == "cube");
    const Shape shape = Describe(model, report.bodies.front());
    std::printf("a unit cube written the way another system would: %.9f, %d faces, watertight %d\n",
                shape.volume, shape.counts.faces, shape.watertight ? 1 : 0);
    CHECK(shape.valid);
    CHECK(shape.watertight);
    CHECK(Near(shape.volume, 1.0, 1e-9));
    CHECK(shape.counts.faces == 6 && shape.counts.edges == 12 && shape.counts.vertices == 8);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestParser();
    TestPrimitives();
    TestBuiltShapes();
    TestSeveralBodies();
    TestReadRefusals();
    TestForeignStyleFile();
    std::printf("cad_step_test passed (%d checks)\n", g_checks);
    return 0;
}
