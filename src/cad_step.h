#ifndef MEP_CAD_STEP_H
#define MEP_CAD_STEP_H

#include "cad_topology.h"

#include <map>
#include <memory>
#include <string>
#include <vector>

// STEP: ISO 10303 part 21 files (plans/CAD_FEM_PLAN.md Parts F.2 and F.3).
//
// TWO LAYERS, DELIBERATELY SEPARATE. The first is the physical file
// format -- ISO 10303-21, which is a small regular grammar of entity
// instances, lists, strings, enumerations and references, and knows
// nothing about geometry. The second maps those entities onto the B-rep
// in cad_topology.h. Keeping them apart matters because almost every
// problem with a real STEP file is in the second layer: the file parses
// and then says something the reader was not expecting. A parser that
// had geometry mixed into it would report those as syntax errors.
//
// READ BEFORE WRITE, and the reason is in the plan: a corpus of real
// files is the most demanding test this kernel will face. Nothing else
// produces the shapes that fall out of twenty years of other people's
// modellers.
//
// WHAT IS SUPPORTED, precisely. The geometry and topology of
// ADVANCED_BREP_SHAPE_REPRESENTATION: cartesian points, directions,
// placements, lines, circles, ellipses and B-spline curves; planes,
// cylinders, cones, spheres, tori and B-spline surfaces; vertices,
// edges, oriented edges, edge loops, faces, shells and solids. Rational
// and non-rational B-splines both. Everything outside that is reported
// by name, with the entity's line in the file, rather than skipped.
namespace cad {

// --- The physical file --------------------------------------------------

// One parameter of an entity instance. Part 21 has exactly these kinds
// and no others, which is why this can be a closed set rather than a
// variant of everything.
struct StepValue {
    enum class Kind {
        Unset,       // $
        Derived,     // *
        Integer,
        Real,
        String,      // '...'
        Enumeration, // .T. .F. .SOMETHING.
        Binary,      // "0..."
        Reference,   // #123
        List,        // (...)
        Typed,       // NAME(...), a select or a simple-type wrapper
    };
    Kind kind = Kind::Unset;
    long long integer = 0;
    double real = 0.0;
    std::string text;              // String, Enumeration, Binary, and Typed's name
    int reference = 0;             // Reference
    std::vector<StepValue> items;  // List, and Typed's arguments

    bool IsNumber() const { return kind == Kind::Integer || kind == Kind::Real; }
    double AsDouble() const { return kind == Kind::Integer ? static_cast<double>(integer) : real; }
};

struct StepEntity {
    int id = 0;
    std::string type;
    std::vector<StepValue> arguments;
    // Where it was, so a complaint about it can say where to look.
    int line = 0;
    // A complex instance -- `#1 = (A(..) B(..) C(..))` -- is how STEP
    // expresses multiple inheritance, and the rational B-splines arrive
    // this way. The parts are kept whole rather than merged, because
    // which part an argument belongs to is the only way to read it.
    std::vector<StepEntity> parts;
    bool IsComplex() const { return !parts.empty(); }
};

struct StepFile {
    // The HEADER section's entities, in order: FILE_DESCRIPTION,
    // FILE_NAME, FILE_SCHEMA.
    std::vector<StepEntity> header;
    // The DATA section, by instance id.
    std::map<int, StepEntity> entities;

    const StepEntity *Get(int id) const;
    // Every entity of a type, in id order.
    std::vector<const StepEntity *> OfType(const std::string &type) const;
    std::string SchemaName() const;
};

// Parses the physical file. Failures name the line.
bool ParseStepFile(const std::string &text, StepFile *out, std::string *error);
// Writes one back out. Header fields that are empty get a plausible
// default, since a STEP file with no FILE_NAME is rejected by most
// readers even though the schema allows it.
std::string WriteStepFile(const StepFile &file, const std::string &schema);

// --- The shapes ----------------------------------------------------------

struct StepReadOptions {
    Tolerance tolerance;
    // Build p-curves for the faces as they are read. On by default: a
    // body without them cannot be tessellated or validated, and a reader
    // that hands back something unusable is not much of a reader.
    bool build_pcurves = true;
    // Refuse a body that does not validate. Off by default, deliberately:
    // real files contain solids that are very slightly open, and a
    // reader that throws those away is less useful than one that hands
    // them over with the report attached.
    bool require_valid = false;
};

struct StepReadReport {
    bool ok = false;
    std::string error;
    // One body per MANIFOLD_SOLID_BREP (or shell-based model) found.
    std::vector<EntityId> bodies;
    std::vector<std::string> body_names;
    // Entities named in the file that this reader does not map. Each is
    // reported once, with a count, because a file with four hundred
    // unsupported trimmed curves should say so in one line.
    std::map<std::string, int> unsupported;
    // Problems that did not stop the read: a face whose bounds could not
    // be built, an edge with no curve. Same reasoning as `unsupported` --
    // a partly-read solid is usually worth having, and saying nothing
    // about what was dropped is not.
    std::vector<std::string> warnings;
};

// Reads every solid in the file into `out`.
bool ReadStepShapes(const StepFile &file, Model *out, StepReadReport *report,
                    const StepReadOptions &options = {});
// The whole way, from text.
bool ReadStepText(const std::string &text, Model *out, StepReadReport *report,
                  const StepReadOptions &options = {});

struct StepWriteOptions {
    std::string author = "mep";
    std::string organisation;
    std::string description = "mep CAD export";
    // The name the part carries in the file's product structure. Empty
    // means "mep".
    std::string product;
    // AP214 by default, which is what most receivers expect for a solid
    // whose product structure is a single part.
    std::string schema = "AUTOMOTIVE_DESIGN { 1 0 10303 214 3 1 1 }";
};

// Writes the named bodies out. Analytic surfaces stay analytic: a
// cylinder is written as a CYLINDRICAL_SURFACE, not as a B-spline that
// happens to be round, because the receiving system's own fillet and
// draft operations depend on knowing that.
bool WriteStepShapes(const Model &model, const std::vector<EntityId> &bodies, std::string *out,
                     std::string *error, const StepWriteOptions &options = {});

}  // namespace cad

#endif
