#include "cad_fem_methods.h"

#include <set>
#include <sstream>

// Part K's method table. See cad_fem_methods.h for why it is here rather
// than beside the session that implements it.
namespace cadfem {
namespace {

// Which calls change nothing. A list rather than a flag written into
// each entry above, because it is short, because it reads as one
// statement about the surface, and because a flag buried in a
// twenty-line initialiser is the kind of thing that gets copied wrong
// when an entry is duplicated to make the next one.
bool ReadOnly(const std::string &name) {
    static const std::set<std::string> read_only = {
        "part.list",    "part.info",  "part.faces", "part.edges", "part.face_at",
        "part.edge_at", "part.mass",  "part.validate",
        "fem.summary", "fem.field", "fem.probe", "fem.path",    "fem.animate", "fem.nodes",
    };
    return read_only.count(name) > 0;
}

const std::vector<Method> &BuildMethods() {
    static const std::vector<Method> methods = [] {
        std::vector<Method> built = {
        // --- Documents and primitives ------------------------------------
        {"part.new", "Opens an empty CAD document and returns its handle.",
         {{"title", "string", false, "A name for the document."}},
         "{document}"},
        {"part.close", "Closes a document and everything built on it.",
         {{"document", "number", true, "The document handle."}},
         "{closed}"},
        {"part.list", "Lists open documents, studies, meshes and results.", {},
         "{documents, studies, meshes, results}"},
        {"part.box", "Adds a rectangular block.",
         {{"document", "number", true, "The document handle."},
          {"size", "array", true, "Its extent, [x, y, z]."},
          {"at", "array", false, "The minimum corner, [x, y, z]. Defaults to the origin."}},
         "{body}"},
        {"part.cylinder", "Adds a cylinder.",
         {{"document", "number", true, "The document handle."},
          {"radius", "number", true, "Its radius."},
          {"height", "number", true, "Its height along the axis."},
          {"at", "array", false, "The centre of the base. Defaults to the origin."},
          {"axis", "array", false, "The axis direction. Defaults to +z."}},
         "{body}"},
        {"part.sphere", "Adds a sphere.",
         {{"document", "number", true, "The document handle."},
          {"radius", "number", true, "Its radius."},
          {"at", "array", false, "Its centre. Defaults to the origin."}},
         "{body}"},
        {"part.boolean", "Combines two bodies and replaces them with the result.",
         {{"document", "number", true, "The document handle."},
          {"op", "string", true, "One of union, difference, intersection."},
          {"a", "number", true, "The first body."},
          {"b", "number", true, "The second body."}},
         "{body}"},
        {"part.shell", "Hollows a body out to a wall thickness.",
         {{"document", "number", true, "The document handle."},
          {"body", "number", true, "The body to hollow."},
          {"thickness", "number", true, "The wall thickness."},
          {"open_faces", "array", false, "Face handles to leave open."}},
         "{body}"},
        // --- Interrogation ------------------------------------------------
        {"part.info", "Counts what a document holds.",
         {{"document", "number", true, "The document handle."}},
         "{bodies, faces, edges, vertices}"},
        {"part.faces", "Lists a body's faces: a normal, and where each one actually is.",
         {{"document", "number", true, "The document handle."},
          {"body", "number", true, "The body."}},
         "{faces: [{id, point, normal, centroid, low, high}]}"},
        {"part.edges", "Lists a body's edges, with a point, a direction and a length on each.",
         {{"document", "number", true, "The document handle."},
          {"body", "number", true, "The body."},
          {"along", "array", false, "Keep only the edges running along this direction."}},
         "{edges: [{id, point, direction, from, to, length}]}"},
        {"part.edge_at", "Finds the edge nearest a point, which is how a script names one.",
         {{"document", "number", true, "The document handle."},
          {"body", "number", true, "The body."},
          {"point", "array", true, "Look for the edge nearest here."},
          {"along", "array", false, "Consider only edges running along this direction."}},
         "{edge, point, direction, from, to, length, distance}"},
        {"part.face_at", "Finds the face whose outward normal is nearest a direction.",
         {{"document", "number", true, "The document handle."},
          {"body", "number", true, "The body."},
          {"normal", "array", true, "The direction to look along."}},
         "{face, point, normal, agreement}"},
        {"part.mass", "Volume, area, centre of mass and inertia of a body.",
         {{"document", "number", true, "The document handle."},
          {"body", "number", true, "The body."},
          {"density", "number", false, "kg/m^3. Defaults to 1, so mass equals volume."}},
         "{volume, area, mass, centroid, inertia, principal}"},
        {"part.validate", "Checks a body's topology and geometry.",
         {{"document", "number", true, "The document handle."},
          {"body", "number", true, "The body."}},
         "{ok, problems}"},
        // --- Files ----------------------------------------------------------
        {"part.export", "Writes a body to STEP, STL, OBJ or glTF.",
         {{"document", "number", true, "The document handle."},
          {"path", "string", true, "Where to write it."},
          {"format", "string", false, "step, stl, obj or gltf. Defaults to the extension."},
          {"body", "number", false, "One body, or every body if omitted."}},
         "{path, bytes, format}"},
        {"part.import", "Reads a STEP file, or a .mepcad feature tree, into a new document.",
         {{"path", "string", true, "The file to read."},
          {"format", "string", false, "step or mepcad. Defaults to the extension."}},
         "{document, bodies, warnings, features, volume}"},
        // --- Meshing and studies ---------------------------------------------
        {"fem.mesh", "Meshes a body, into tetrahedra or by sweeping a profile into hexahedra.",
         {{"document", "number", true, "The document handle."},
          {"body", "number", true, "The body to mesh."},
          {"size", "number", false, "Target element size. Defaults to the model's own scale."},
          {"order", "number", false, "1 for Tet4, 2 for curved Tet10. Defaults to 1."},
          {"method", "string", false,
           "tetrahedra (the default) or sweep, which needs a prismatic body and gives Hex8."},
          {"layers", "number", false, "Elements along a sweep. Defaults to the sizing field."}},
         "{mesh, nodes, elements, shape, order, worst_dihedral, target_size}"},
        {"fem.study", "Starts a study on a document.",
         {{"document", "number", true, "The document handle."},
          {"name", "string", false, "A name for it."}},
         "{study}"},
        {"fem.material", "Sets the study's material.",
         {{"study", "number", true, "The study handle."},
          {"youngs_modulus", "number", false, "Pa. Defaults to steel."},
          {"poissons_ratio", "number", false, "Defaults to 0.3."},
          {"density", "number", false, "kg/m^3. Defaults to 7850."},
          {"thermal_expansion", "number", false, "1/K."},
          {"name", "string", false, "A name for the material."}},
         "{materials}"},
        {"fem.support", "Holds a face, or an edge -- which is what a simple support is.",
         {{"study", "number", true, "The study handle."},
          {"face", "number", false, "The face to hold."},
          {"edge", "number", false, "The edge to hold, instead of a face."},
          {"fixed", "array", false, "Which of x, y, z are held. Defaults to all three."}},
         "{supports}"},
        {"fem.load", "Applies a load to a face, or gravity to the whole model.",
         {{"study", "number", true, "The study handle."},
          {"kind", "string", true, "force, pressure, traction or gravity."},
          {"face", "number", false, "The face, for everything but gravity."},
          {"magnitude", "number", false, "Pa, for a pressure."},
          {"vector", "array", false, "The force, traction or acceleration."}},
         "{loads}"},
        // --- Solving ----------------------------------------------------------
        {"fem.solve", "Runs a linear static solve.",
         {{"study", "number", true, "The study handle."},
          {"mesh", "number", true, "The mesh handle."}},
         "{result, nodes, elements, max_displacement, max_von_mises, strain_energy,"
         " equilibrium_residual, relative_error}"},
        {"fem.modal", "Finds natural frequencies and mode shapes.",
         {{"study", "number", true, "The study handle."},
          {"mesh", "number", true, "The mesh handle."},
          {"modes", "number", false, "How many. Defaults to 6."}},
         "{result, frequencies, rigid_body_modes}"},
        {"fem.adapt", "Meshes, solves, estimates the error and refines, repeatedly.",
         {{"study", "number", true, "The study handle."},
          {"body", "number", true, "The body to mesh each cycle."},
          {"size", "number", false, "The starting element size."},
          {"target", "number", false, "The relative energy-norm error to aim for."},
          {"cycles", "number", false, "How many at most. Defaults to 3."}},
         "{result, cycles, reached_target, stopped_early, stop_reason}"},
        // --- Reading results ----------------------------------------------------
        {"fem.summary", "Everything scalar about a result.",
         {{"result", "number", true, "The result handle."}},
         "{nodes, elements, max_displacement, max_von_mises, strain_energy, relative_error}"},
        {"fem.field", "The range of one field over a result.",
         {{"result", "number", true, "The result handle."},
          {"field", "string", true, "von_mises, displacement, max_principal and so on."}},
         "{field, min, max, min_node, max_node, at_min, at_max}"},
        {"fem.probe", "Reads a field at points.",
         {{"result", "number", true, "The result handle."},
          {"field", "string", true, "Which field."},
          {"points", "array", true, "An array of [x, y, z]."}},
         "{probes: [{at, value, inside, distance}]}"},
        {"fem.path", "Samples a field along a line, for plotting.",
         {{"result", "number", true, "The result handle."},
          {"field", "string", true, "Which field."},
          {"from", "array", true, "The start point."},
          {"to", "array", true, "The end point."},
          {"samples", "number", false, "How many. Defaults to 20."}},
         "{rows: [{distance, at, value}], csv}"},
        {"fem.animate", "Frames of a mode shape, as displacement per node.",
         {{"result", "number", true, "A result from fem.modal."},
          {"mode", "number", false, "Which mode, from zero. Defaults to the first."},
          {"frames", "number", false, "How many. Defaults to 24."},
          {"amplitude", "number", false, "Peak movement as a fraction of the diagonal."}},
         "{frames, period, amplitude_scale, displacement}"},
        {"fem.nodes", "The result's node coordinates, three numbers per node.",
         {{"result", "number", true, "The result handle."}},
         "{count, nodes}"},
        {"fem.movie", "Renders a result as a playable Motion-JPEG .mov.",
         {{"result", "number", true, "The result handle."},
          {"path", "string", true, "Where to write the film."},
          {"field", "string", false, "Which field colours it. Defaults to von_mises,"
                                     " or displacement for a modal result."},
          {"mode", "number", false, "For a modal result, which mode. Defaults to the first."},
          {"frames", "number", false, "How many. Defaults to 36."},
          {"fps", "number", false, "Frames per second. Defaults to 20."},
          {"width", "number", false, "Pixels. Defaults to 720."},
          {"height", "number", false, "Pixels. Defaults to 540."},
          {"amplitude", "number", false, "Peak movement as a fraction of the diagonal."},
          {"yaw", "number", false, "Camera bearing in degrees."},
          {"pitch", "number", false, "Camera elevation in degrees."},
          {"orbit", "bool", false, "Turn the camera a full circle over the film."},
          {"caption", "string", false, "A line of text along the top."},
          {"color_map", "string", false, "viridis, blue_to_red or greyscale."}},
         "{path, frames, seconds, width, height, field_min, field_max, bytes}"},
        {"fem.image", "Renders one frame of a result as a PNG.",
         {{"result", "number", true, "The result handle."},
          {"path", "string", true, "Where to write the picture."},
          {"field", "string", false, "Which field colours it."},
          {"mode", "number", false, "For a modal result, which mode to draw at its extreme."},
          {"width", "number", false, "Pixels. Defaults to 720."},
          {"height", "number", false, "Pixels. Defaults to 540."},
          {"amplitude", "number", false, "Peak movement as a fraction of the diagonal."},
          {"yaw", "number", false, "Camera bearing in degrees."},
          {"pitch", "number", false, "Camera elevation in degrees."},
          {"caption", "string", false, "A line of text along the top."},
          {"color_map", "string", false, "viridis, blue_to_red or greyscale."}},
         "{path, width, height, field_min, field_max, bytes}"},
        {"fem.export", "Writes a path plot or a field to CSV or an org table.",
         {{"result", "number", true, "The result handle."},
          {"field", "string", true, "Which field."},
          {"path", "string", true, "Where to write it."},
          {"format", "string", false, "csv or org. Defaults to the extension."},
          {"from", "array", false, "A path start; omit to export every node."},
          {"to", "array", false, "A path end."},
          {"samples", "number", false, "Path samples."}},
         "{path, rows, format}"},
        };
        for (Method &method : built) method.read_only = ReadOnly(method.name);
        return built;
    }();
    return methods;
}

}  // namespace

const std::vector<Method> &Methods() { return BuildMethods(); }

const Method *FindMethod(const std::string &name) {
    for (const Method &method : Methods()) {
        if (name == method.name) return &method;
    }
    return nullptr;
}

namespace {

// `part.box` -> `mep_part_box`, the MCP tool name; and `mep.part_box`, the
// Lua one. Both are mechanical from the method name, which is why the
// documentation can state the rule once instead of listing three names
// per entry.
std::string Underscored(const std::string &method) {
    std::string out = method;
    for (char &ch : out) {
        if (ch == '.') ch = '_';
    }
    return out;
}

std::string Signature(const Method &method) {
    std::string out = Underscored(method.name) + "(";
    bool first = true;
    for (const Parameter &parameter : method.params) {
        if (!first) out += ", ";
        first = false;
        out += parameter.name;
        if (!parameter.required) out += "?";
    }
    return out + ")";
}

}  // namespace

std::string MarkdownReference() {
    std::ostringstream out;
    out << "The CAD kernel and finite-element solver, as "
        << Methods().size() << " methods. Each one is reachable three ways, with\n"
        << "the name mechanically derived from the method: `part.box` is the agent-RPC\n"
        << "method, `mep_part_box` the MCP tool, and `mep.part_box` the Lua function. All\n"
        << "three go to one implementation (`src/cad_fem_api.cpp`) driven by one table\n"
        << "(`src/cad_fem_methods.cpp`), and this section is generated from that table --\n"
        << "run `just cad-fem-docs` after changing it.\n\n"
        << "Handles are integers and share one counter across documents, studies, meshes\n"
        << "and results, so a handle of one kind is never mistaken for another. Closing a\n"
        << "document closes everything built on it.\n";
    std::string section;
    for (const Method &method : Methods()) {
        const std::string prefix = std::string(method.name).substr(0, 3);
        const std::string heading = prefix == "cad" ? "Geometry" : "Analysis";
        if (heading != section) {
            section = heading;
            out << "\n### " << heading << "\n\n";
        }
        out << "- `" << Signature(method) << "`"
            << (method.read_only ? " (read-only)" : "") << " -- " << method.summary << "\n";
        for (const Parameter &parameter : method.params) {
            out << "  - `" << parameter.name << "` (" << parameter.type
                << (parameter.required ? ", required" : ", optional") << ") -- "
                << parameter.summary << "\n";
        }
        out << "  - returns `" << method.returns << "`\n";
    }
    return out.str();
}

std::string OrgReference() {
    std::ostringstream out;
    std::string section;
    for (const Method &method : Methods()) {
        const std::string prefix = std::string(method.name).substr(0, 3);
        const std::string heading = prefix == "cad" ? "Geometry" : "Analysis";
        if (heading != section) {
            section = heading;
            out << "\n** " << heading << "\n\n";
        }
        out << "*** " << Signature(method) << "\n";
        if (method.read_only) out << "Read-only.\n";
        out << method.summary << "\n\n";
        if (!method.params.empty()) {
            out << "| parameter | type | | meaning |\n|---|---|---|---|\n";
            for (const Parameter &parameter : method.params) {
                out << "| =" << parameter.name << "= | " << parameter.type << " | "
                    << (parameter.required ? "required" : "optional") << " | "
                    << parameter.summary << " |\n";
            }
            out << "\n";
        }
        out << "Returns =" << method.returns << "=.\n";
    }
    return out.str();
}

}  // namespace cadfem
