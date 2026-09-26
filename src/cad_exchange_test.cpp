// Windowless coverage for cad_exchange.h and cad_drawing.h (Part F.4).
//
// AN EXPORTER'S ONLY HONEST CLAIM is that what comes out describes the
// same shape, so every writer here is checked by reading its output back
// and measuring it. STL round-trips through its own reader; OBJ and glTF
// are checked by counting what they say they contain and by recovering
// the geometry from the text; DXF goes out and comes back as a sketch
// whose entities are in the same places.
//
// The drawing view is checked against things that are true of a drawing
// rather than against a picture: a cube seen square-on has four visible
// edges and four hidden ones behind them, seen from a corner it has nine
// visible and three hidden, and a cylinder's outline contains two
// straight silhouettes that are not edges of the body at all.

#include "cad_drawing.h"
#include "cad_exchange.h"

#include "cad_pcurve.h"
#include "cad_validate.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <map>
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

Model Box(const Vec3d &corner, const Vec3d &size, EntityId *body) {
    Model model;
    std::string error;
    Check(MakeBox(corner, size, &model, body), "MakeBox", __LINE__);
    Check(BuildAllPCurves(&model, {}, &error), "BuildAllPCurves", __LINE__);
    return model;
}

// --- Tessellated exports ---------------------------------------------------

void TestStl() {
    EntityId body = kNoEntity;
    const Model model = Box(Vec3d{0.0, 0.0, 0.0}, Vec3d{2.0, 3.0, 4.0}, &body);
    std::string error;

    for (bool ascii : {false, true}) {
        MeshExportOptions options;
        options.ascii_stl = ascii;
        options.name = "block";
        std::string text;
        CHECK_MESSAGE(WriteStl(model, {body}, &text, &error, options), error);
        TessellationMesh mesh;
        CHECK_MESSAGE(ReadStl(text, &mesh, &error), error);
        CHECK(mesh.TriangleCount() == 12);
        CHECK(mesh.IsClosed());
        CHECK(Near(mesh.SignedVolume(), 24.0, 1e-5));
        std::printf("STL %s: %d triangles, watertight, %.6f\n", ascii ? "ascii " : "binary",
                    mesh.TriangleCount(), mesh.SignedVolume());
    }

    // A binary STL must not start with "solid", or every reader in the
    // world takes it for the ASCII form.
    std::string binary;
    CHECK_MESSAGE(WriteStl(model, {body}, &binary, &error, {}), error);
    CHECK(binary.compare(0, 5, "solid") != 0);
    CHECK(binary.size() == 84 + 12 * 50);

    // Two bodies in one file are one mesh.
    EntityId second = kNoEntity;
    Model pair = Box(Vec3d{0.0, 0.0, 0.0}, Vec3d{1.0, 1.0, 1.0}, &body);
    CHECK(MakeBox(Vec3d{10.0, 0.0, 0.0}, Vec3d{2.0, 2.0, 2.0}, &pair, &second));
    CHECK(BuildAllPCurves(&pair, {}, &error));
    std::string both;
    CHECK_MESSAGE(WriteStl(pair, {body, second}, &both, &error, {}), error);
    TessellationMesh mesh;
    CHECK_MESSAGE(ReadStl(both, &mesh, &error), error);
    CHECK(mesh.TriangleCount() == 24);
    CHECK(Near(mesh.SignedVolume(), 1.0 + 8.0, 1e-5));

    CHECK(!WriteStl(model, {}, &both, &error, {}));
    CHECK(error.find("no bodies") != std::string::npos);
    TessellationMesh broken;
    CHECK(!ReadStl("too short", &broken, &error));
}

void TestObjAndGltf() {
    EntityId body = kNoEntity;
    const Model model = Box(Vec3d{0.0, 0.0, 0.0}, Vec3d{2.0, 3.0, 4.0}, &body);
    std::string error;

    std::string obj;
    CHECK_MESSAGE(WriteObj(model, {body}, &obj, &error, {}), error);
    int vertices = 0;
    int normals = 0;
    int faces = 0;
    int smallest_index = 1 << 30;
    std::size_t at = 0;
    while (at < obj.size()) {
        std::size_t newline = obj.find('\n', at);
        if (newline == std::string::npos) newline = obj.size();
        const std::string line = obj.substr(at, newline - at);
        at = newline + 1;
        if (line.compare(0, 2, "v ") == 0) ++vertices;
        if (line.compare(0, 3, "vn ") == 0) ++normals;
        if (line.compare(0, 2, "f ") == 0) {
            ++faces;
            int a = 0;
            int b = 0;
            int c = 0;
            if (std::sscanf(line.c_str(), "f %d//%*d %d//%*d %d//%*d", &a, &b, &c) == 3) {
                smallest_index = std::min(smallest_index, std::min(a, std::min(b, c)));
            }
        }
    }
    CHECK(faces == 12);
    CHECK(vertices == normals);
    // OBJ indexes from one, which is the single most common thing to get
    // wrong about it, so it is checked rather than assumed.
    CHECK(smallest_index == 1);
    std::printf("OBJ: %d vertices, %d normals, %d faces, indices from %d\n", vertices, normals, faces,
                smallest_index);

    std::string gltf;
    CHECK_MESSAGE(WriteGltf(model, {body}, &gltf, &error, {}), error);
    CHECK(gltf.find("\"version\":\"2.0\"") != std::string::npos);
    CHECK(gltf.find("data:application/octet-stream;base64,") != std::string::npos);
    // The accessors have to agree with the mesh: 36 indices for 12
    // triangles, and as many positions as normals.
    CHECK(gltf.find("\"count\":36") != std::string::npos);
    CHECK(gltf.find("\"componentType\":5125") != std::string::npos);
    // And the declared buffer length has to be the base64's real length,
    // which is what a viewer checks first.
    const std::size_t length_at = gltf.find("\"byteLength\":");
    CHECK(length_at != std::string::npos);
    const long long declared = std::atoll(gltf.c_str() + length_at + 13);
    const std::size_t data_at = gltf.find("base64,") + 7;
    const std::size_t data_end = gltf.find('"', data_at);
    const std::size_t encoded = data_end - data_at;
    // Four base64 characters per three bytes, rounded up, with padding.
    CHECK(encoded == (static_cast<std::size_t>(declared) + 2) / 3 * 4);
    std::printf("glTF: buffer of %lld bytes, %d base64 characters, accessors agree\n", declared,
                static_cast<int>(encoded));
}

// --- DXF --------------------------------------------------------------------

SketchPlane AxisAlignedPlane() {
    SketchPlane plane = PlaneFromNormal(Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 1.0});
    plane.x_axis = Vec3d{1.0, 0.0, 0.0};
    plane.y_axis = Vec3d{0.0, 1.0, 0.0};
    return plane;
}

void TestDxf() {
    Sketch sketch(AxisAlignedPlane());
    const SketchId a = sketch.AddPoint(Vec2d{0.0, 0.0});
    const SketchId b = sketch.AddPoint(Vec2d{10.0, 0.0});
    const SketchId c = sketch.AddPoint(Vec2d{10.0, 6.0});
    const SketchId d = sketch.AddPoint(Vec2d{0.0, 6.0});
    sketch.AddLineFromPoints(a, b);
    sketch.AddLineFromPoints(b, c);
    sketch.AddLineFromPoints(c, d);
    sketch.AddLineFromPoints(d, a);
    sketch.AddCircle(Vec2d{3.0, 3.0}, 1.25);
    sketch.AddArc(Vec2d{7.0, 3.0}, Vec2d{8.0, 3.0}, Vec2d{7.0, 4.0}, true);
    sketch.AddLine(Vec2d{0.0, 3.0}, Vec2d{10.0, 3.0}, true);  // construction

    std::string text;
    std::string error;
    CHECK_MESSAGE(WriteDxf(sketch, &text, &error), error);
    CHECK(text.find("ENTITIES") != std::string::npos);
    CHECK(text.find("LWPOLYLINE") == std::string::npos);
    CHECK(text.find("\nCIRCLE\n") != std::string::npos);
    CHECK(text.find("\nARC\n") != std::string::npos);
    CHECK(text.find("construction") != std::string::npos);

    Sketch back;
    std::vector<std::string> unsupported;
    CHECK_MESSAGE(ReadDxf(text, AxisAlignedPlane(), &back, &unsupported, &error), error);
    CHECK(unsupported.empty());
    int lines = 0;
    int circles = 0;
    int arcs = 0;
    for (const SketchEntity &entity : back.Entities()) {
        if (entity.kind == SketchEntityKind::Line) ++lines;
        if (entity.kind == SketchEntityKind::Circle) ++circles;
        if (entity.kind == SketchEntityKind::Arc) ++arcs;
    }
    CHECK(lines == 5);
    CHECK(circles == 1);
    CHECK(arcs == 1);
    std::printf("DXF: %d lines, %d circles, %d arcs out and back\n", lines, circles, arcs);

    // The geometry is where it was. Circles are easy to check by centre
    // and radius.
    bool found = false;
    for (const SketchEntity &entity : back.Entities()) {
        if (entity.kind != SketchEntityKind::Circle) continue;
        const SketchPoint *centre = back.GetPoint(entity.points[0]);
        CHECK(centre != nullptr);
        CHECK(Near(centre->position.x, 3.0, 1e-9) && Near(centre->position.y, 3.0, 1e-9));
        CHECK(Near(entity.scalars[0], 1.25, 1e-9));
        found = true;
    }
    CHECK(found);

    // A closed LWPOLYLINE becomes a closed run of lines, which is how
    // most DXF profiles in the wild are written.
    const std::string polyline =
        "0\nSECTION\n2\nENTITIES\n0\nLWPOLYLINE\n8\n0\n90\n4\n70\n1\n"
        "10\n0.0\n20\n0.0\n10\n4.0\n20\n0.0\n10\n4.0\n20\n2.0\n10\n0.0\n20\n2.0\n"
        "0\nENDSEC\n0\nEOF\n";
    Sketch from_polyline;
    CHECK_MESSAGE(ReadDxf(polyline, AxisAlignedPlane(), &from_polyline, &unsupported, &error), error);
    int polyline_lines = 0;
    for (const SketchEntity &entity : from_polyline.Entities()) {
        if (entity.kind == SketchEntityKind::Line) ++polyline_lines;
    }
    CHECK(polyline_lines == 4);
    std::printf("a closed LWPOLYLINE comes in as %d lines\n", polyline_lines);

    // An entity this reader does not map is named and counted.
    const std::string exotic =
        "0\nSECTION\n2\nENTITIES\n0\nMTEXT\n1\nhello\n0\nHATCH\n0\nHATCH\n0\nENDSEC\n0\nEOF\n";
    Sketch from_exotic;
    CHECK_MESSAGE(ReadDxf(exotic, AxisAlignedPlane(), &from_exotic, &unsupported, &error), error);
    CHECK(unsupported.size() == 2);
    std::printf("unsupported DXF entities: %s, %s\n", unsupported[0].c_str(), unsupported[1].c_str());

    Sketch nothing;
    CHECK(!ReadDxf("", AxisAlignedPlane(), &nothing, &unsupported, &error));
    CHECK(error.find("not DXF") != std::string::npos);
}

// --- IGES --------------------------------------------------------------------

// A hand-written IGES file is the only way to test the reader without
// shipping one, and writing it by hand is the point: it is what forces
// the eighty-column layout and the odd-numbered directory pointers to be
// got right rather than assumed.
std::string IgesLine(const std::string &content, char section, int sequence) {
    std::string line = content;
    line.resize(72, ' ');
    line.push_back(section);
    char number[16];
    std::snprintf(number, sizeof(number), "%7d", sequence);
    line += number;
    return line + "\n";
}

void TestIges() {
    // A tetrahedron: four vertices, six edges, four planar faces. Small
    // enough to write out by hand and big enough to exercise the
    // directory, the parameter records and the pointer arithmetic.
    std::string file;
    file += IgesLine("mep test file", 'S', 1);
    file += IgesLine("1H,,1H;,4Hmep,8Htest.igs,4Hmep,4Hmep,32,38,6,308,15,4Hmep,1.,1,", 'G', 1);
    file += IgesLine("2HMM,1,0.1,13H000101.000000,1.E-7,1.,4Hmep,4Hmep,11,0;", 'G', 2);

    // The directory. Two lines per entry; the pointer to an entry is its
    // first line's number, so they are 1, 3, 5, ...
    struct Entry {
        int type;
        int parameter_line;
        int line_count;
        int form;
    };
    const Entry entries[] = {
        {502, 1, 1, 1},   // #1  vertex list
        {110, 2, 1, 0},   // #3  line (reused as geometry for every edge)
        {504, 3, 1, 1},   // #5  edge list
        {508, 4, 1, 1},   // #7  loop 0
        {508, 5, 1, 1},   // #9  loop 1
        {508, 6, 1, 1},   // #11 loop 2
        {508, 7, 1, 1},   // #13 loop 3
        {190, 8, 1, 0},   // #15 plane 0
        {190, 9, 1, 0},   // #17 plane 1
        {190, 10, 1, 0},  // #19 plane 2
        {190, 11, 1, 0},  // #21 plane 3
        {510, 12, 1, 1},  // #23 face 0
        {510, 13, 1, 1},  // #25 face 1
        {510, 14, 1, 1},  // #27 face 2
        {510, 15, 1, 1},  // #29 face 3
        {514, 16, 1, 1},  // #31 shell
        {186, 17, 1, 0},  // #33 manifold solid
        {116, 18, 1, 0},  // #35 point, for the planes
        {123, 19, 1, 0},  // #37 direction
    };
    int sequence = 1;
    for (const Entry &entry : entries) {
        char first[128];
        std::snprintf(first, sizeof(first), "%8d%8d%8d%8d%8d%8d%8d%8d%8d", entry.type,
                      entry.parameter_line, 0, 0, 0, 0, 0, 0, 0);
        file += IgesLine(first, 'D', sequence++);
        char second[128];
        std::snprintf(second, sizeof(second), "%8d%8d%8d%8d%8d%8s%8s%8s", entry.type, 0, 0,
                      entry.line_count, entry.form, "", "", "solid");
        file += IgesLine(second, 'D', sequence++);
    }

    // The parameter records. Each line carries its owner's directory
    // pointer in columns 65-72.
    // A parameter record longer than 64 columns continues onto another
    // line, each carrying the same directory pointer. Real files do this
    // constantly -- a B-spline's record runs to dozens of lines -- so the
    // test writes it properly rather than truncating, which is what the
    // first version of this did and is why the reader's continuation
    // handling is exercised at all.
    int p = 1;
    auto parameter = [&](const std::string &content, int owner) {
        std::string out;
        for (std::size_t at = 0; at < content.size(); at += 64) {
            std::string line = content.substr(at, 64);
            line.resize(64, ' ');
            char number[16];
            std::snprintf(number, sizeof(number), "%8d", owner);
            line += number;
            out += IgesLine(line, 'P', p++);
        }
        return out;
    };
    // Four corners of a tetrahedron.
    file += parameter("502,4,0.,0.,0.,1.,0.,0.,0.,1.,0.,0.,0.,1.;", 1);
    file += parameter("110,0.,0.,0.,1.,0.,0.;", 3);
    // Six edges, each (curve, start list, start index, end list, end index).
    file += parameter("504,6,3,1,1,1,2,3,1,2,1,3,3,1,3,1,1,3,1,1,1,4,"
                      "3,1,2,1,4,3,1,3,1,4;", 5);
    // Four loops, three edges each: (type, list, index, orientation, 0).
    file += parameter("508,3,0,5,1,1,0,0,5,2,1,0,0,5,3,0,0;", 7);
    file += parameter("508,3,0,5,1,0,0,0,5,4,1,0,0,5,5,0,0;", 9);
    file += parameter("508,3,0,5,2,0,0,0,5,5,1,0,0,5,6,0,0;", 11);
    file += parameter("508,3,0,5,3,1,0,0,5,6,1,0,0,5,4,0,0;", 13);
    // Four planes, all referring to the same point and direction, which
    // is wrong geometrically and irrelevant here: the reader is being
    // asked whether it walks the structure, and the planes it builds are
    // never evaluated because the test does not tessellate.
    for (int i = 0; i < 4; ++i) file += parameter("190,35,37,0;", 15 + i * 2);
    for (int i = 0; i < 4; ++i) {
        file += parameter("510," + std::to_string(15 + i * 2) + ",1,1," + std::to_string(7 + i * 2) + ";",
                          23 + i * 2);
    }
    file += parameter("514,4,23,1,25,1,27,1,29,1;", 31);
    file += parameter("186,31,1,0;", 33);
    file += parameter("116,0.,0.,0.,0;", 35);
    file += parameter("123,0.,0.,1.;", 37);
    file += IgesLine("S      3G      2D     38P     17", 'T', 1);

    Model model;
    IgesReadReport report;
    CHECK_MESSAGE(ReadIgesText(file, &model, &report), report.error);
    CHECK(report.bodies.size() == 1);
    const TopologyCounts counts = model.CountsOfBody(report.bodies.front());
    std::printf("IGES: a tetrahedron read as %d faces, %d edges, %d vertices\n", counts.faces,
                counts.edges, counts.vertices);
    CHECK(counts.faces == 4);
    CHECK(counts.edges == 6);
    CHECK(counts.vertices == 4);

    // What it refuses, and what it says about the common case it cannot
    // read.
    Model nothing;
    IgesReadReport empty_report;
    CHECK(!ReadIgesText("not an iges file", &nothing, &empty_report));
    CHECK(empty_report.error.find("column 73") != std::string::npos);
    std::string trimmed = IgesLine("x", 'S', 1) + IgesLine("144,1,1,0,1;", 'P', 1) +
                          IgesLine("     144       1", 'D', 1) + IgesLine("     144", 'D', 2) +
                          IgesLine("", 'T', 1);
    IgesReadReport trimmed_report;
    CHECK(!ReadIgesText(trimmed, &nothing, &trimmed_report));
    std::printf("a trimmed-surface file: %s\n", trimmed_report.error.c_str());
    CHECK(trimmed_report.error.find("144") != std::string::npos);
}

// --- Drawing views -------------------------------------------------------------

void TestDrawingView() {
    EntityId body = kNoEntity;
    const Model model = Box(Vec3d{0.0, 0.0, 0.0}, Vec3d{4.0, 4.0, 4.0}, &body);
    std::string error;

    // Square on to a face: four edges of the near face are visible, the
    // four of the far face are hidden behind them, and the four running
    // away from the viewer project to points.
    DrawingViewOptions options;
    options.direction = Vec3d{0.0, 1.0, 0.0};
    options.up = Vec3d{0.0, 0.0, 1.0};
    options.silhouettes = false;
    DrawingView view;
    CHECK_MESSAGE(MakeDrawingView(model, {body}, options, &view, &error), error);
    std::printf("a cube square on: %d visible, %d hidden segments\n", view.visible, view.hidden);
    CHECK(view.hidden > 0);
    CHECK(view.visible > 0);
    CHECK(Near(view.x.hi - view.x.lo, 4.0, 1e-9));
    CHECK(Near(view.y.hi - view.y.lo, 4.0, 1e-9));

    // Count whole edges rather than segments: an edge is visible if any
    // of its pieces is.
    std::map<EntityId, bool> any_visible;
    std::map<EntityId, bool> any_hidden;
    for (const DrawingSegment &segment : view.segments) {
        if (segment.edge == kNoEntity) continue;
        if (segment.hidden) {
            any_hidden[segment.edge] = true;
        } else {
            any_visible[segment.edge] = true;
        }
    }
    int fully_hidden = 0;
    for (const auto &entry : any_hidden) {
        if (!any_visible.count(entry.first)) ++fully_hidden;
    }
    // The four edges of the far face are entirely behind the near one.
    CHECK(fully_hidden == 4);
    std::printf("  four edges entirely hidden, which is the far face\n");

    // From a corner, more of it shows: only the three edges meeting the
    // far corner are hidden.
    options.direction = Vec3d{1.0, 1.0, 1.0};
    options.up = Vec3d{0.0, 0.0, 1.0};
    DrawingView corner;
    CHECK_MESSAGE(MakeDrawingView(model, {body}, options, &corner, &error), error);
    any_visible.clear();
    any_hidden.clear();
    for (const DrawingSegment &segment : corner.segments) {
        if (segment.edge == kNoEntity) continue;
        if (segment.hidden) {
            any_hidden[segment.edge] = true;
        } else {
            any_visible[segment.edge] = true;
        }
    }
    fully_hidden = 0;
    for (const auto &entry : any_hidden) {
        if (!any_visible.count(entry.first)) ++fully_hidden;
    }
    std::printf("  from a corner: %d edges entirely hidden\n", fully_hidden);
    CHECK(fully_hidden == 3);

    // A cylinder seen from the side: its outline includes two straight
    // lines that are not edges of the body at all.
    Model tube;
    EntityId tube_body = kNoEntity;
    CHECK(MakeCylinder(Vec3d{0.0, 0.0, 0.0}, Vec3d{0.0, 0.0, 1.0}, 2.0, 6.0, &tube, &tube_body));
    CHECK(BuildAllPCurves(&tube, {}, &error));
    DrawingViewOptions side;
    side.direction = Vec3d{0.0, 1.0, 0.0};
    side.up = Vec3d{0.0, 0.0, 1.0};
    side.silhouettes = true;
    DrawingView outline;
    CHECK_MESSAGE(MakeDrawingView(tube, {tube_body}, side, &outline, &error), error);
    int silhouette_segments = 0;
    for (const DrawingSegment &segment : outline.segments) {
        if (segment.edge == kNoEntity && !segment.hidden) ++silhouette_segments;
    }
    std::printf("a cylinder from the side: %d silhouette segments, extent %.3f wide\n",
                silhouette_segments, outline.x.hi - outline.x.lo);
    CHECK(silhouette_segments > 0);
    // The outline is exactly the diameter wide and the height tall.
    CHECK(Near(outline.x.hi - outline.x.lo, 4.0, 1e-2));
    CHECK(Near(outline.y.hi - outline.y.lo, 6.0, 1e-9));

    // And it draws.
    const std::string svg = DrawingViewToSvg(view, 120.0);
    CHECK(svg.compare(0, 4, "<svg") == 0);
    CHECK(svg.find("stroke-dasharray") != std::string::npos);
    CHECK(svg.find("</svg>") != std::string::npos);
    std::printf("SVG: %d bytes, hidden lines dashed\n", static_cast<int>(svg.size()));

    DrawingView nothing;
    CHECK(!MakeDrawingView(model, {}, options, &nothing, &error));
    DrawingViewOptions bad;
    bad.direction = Vec3d{0.0, 0.0, 0.0};
    CHECK(!MakeDrawingView(model, {body}, bad, &nothing, &error));
    CHECK(error.find("no direction") != std::string::npos);
}

}  // namespace

int main() {
    std::setvbuf(stdout, nullptr, _IONBF, 0);
    TestStl();
    TestObjAndGltf();
    TestDxf();
    TestIges();
    TestDrawingView();
    std::printf("cad_exchange_test passed (%d checks)\n", g_checks);
    return 0;
}
