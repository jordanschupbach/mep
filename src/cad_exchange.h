#ifndef MEP_CAD_EXCHANGE_H
#define MEP_CAD_EXCHANGE_H

#include "cad_sketch.h"
#include "cad_tessellate.h"
#include "cad_topology.h"

#include <map>
#include <string>
#include <vector>

// The rest of interchange (plans/CAD_FEM_PLAN.md Part F.4): IGES in, DXF
// both ways, and the tessellated exports.
//
// THE TESSELLATED FORMATS ARE ALL THE SAME FORMAT. STL, OBJ and glTF
// differ in how they spell a triangle and in nothing else that matters
// here, so they share Part B.5's mesh and each is a few dozen lines of
// writing it out. What is worth being careful about is what they lose:
// every one of them throws away the faces, the surfaces and the
// topology, and keeps an approximation whose accuracy is whatever chord
// tolerance it was given. They are for looking at and for printing, and
// the header says so because it is the sort of thing that gets forgotten
// between writing an exporter and using one.
//
// IGES IS READ AND NOT WRITTEN, deliberately. It is still common in
// supply chains, so files arrive; nothing is improved by sending one
// back out when STEP exists and says the same things better.
namespace cad {

// --- Tessellated exports -------------------------------------------------

struct MeshExportOptions {
    TessellationOptions tessellation;
    std::string name = "mep";
    // STL is normally binary; the ASCII form is worth having because it
    // is the one you can look at when something is wrong.
    bool ascii_stl = false;
};

bool WriteStl(const Model &model, const std::vector<EntityId> &bodies, std::string *out,
              std::string *error, const MeshExportOptions &options = {});
bool WriteObj(const Model &model, const std::vector<EntityId> &bodies, std::string *out,
              std::string *error, const MeshExportOptions &options = {});
// glTF 2.0, as a single self-contained .gltf with the buffer inline as a
// base64 data URI. One file is worth more than the few percent a .glb
// would save.
bool WriteGltf(const Model &model, const std::vector<EntityId> &bodies, std::string *out,
               std::string *error, const MeshExportOptions &options = {});

// Reading a mesh back, which is what makes the writers testable: a
// triangle count and a volume that match what went in is the only claim
// an exporter can make.
bool ReadStl(const std::string &text, TessellationMesh *out, std::string *error);

// --- DXF, for 2D sketches -------------------------------------------------

// Writes a sketch's geometry as DXF entities in its own plane. Only the
// geometry: DXF has no way to say "these two lines are perpendicular",
// so the constraints do not survive and a round trip through it is a
// one-way trip out of the parametric world. `.mepcad` is the format that
// keeps them.
bool WriteDxf(const Sketch &sketch, std::string *out, std::string *error);
// Reads LINE, CIRCLE, ARC, LWPOLYLINE and POLYLINE into a sketch on the
// given plane. Everything else is counted and named.
bool ReadDxf(const std::string &text, const SketchPlane &plane, Sketch *out,
             std::vector<std::string> *unsupported, std::string *error);

// --- IGES ------------------------------------------------------------------

struct IgesReadReport {
    bool ok = false;
    std::string error;
    std::vector<EntityId> bodies;
    // IGES entity type numbers that were seen and not mapped, with counts.
    // Numbers rather than names because that is how the file says it and
    // how the specification is indexed.
    std::map<int, int> unsupported;
    std::vector<std::string> warnings;
};

// Reads the B-rep entities of an IGES file: type 186 (manifold solid),
// 514 (shell), 510 (face), 508 (loop), 504 (edge list), 502 (vertex
// list), and the geometry under them.
bool ReadIgesText(const std::string &text, Model *out, IgesReadReport *report);

}  // namespace cad

#endif
