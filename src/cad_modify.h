#ifndef MEP_CAD_MODIFY_H
#define MEP_CAD_MODIFY_H

#include "cad_math.h"
#include "cad_topology.h"

#include <memory>
#include <string>
#include <vector>

// Direct modelling on a finished body (plans/CAD_FEM_PLAN.md Part E.4):
// offset, draft, shell and thicken.
//
// THESE ARE ONE OPERATION WEARING FOUR HATS, and noticing that is what
// makes them small. Each of them changes what surface some faces lie on
// and changes nothing about how the body is connected. Offsetting moves
// every face along its own normal; drafting tilts the faces you name
// about a neutral plane; shelling offsets the faces you keep inwards and
// leaves the ones you remove where they are. In all three the vertices
// and edges then have to be worked out again -- a corner is wherever its
// three faces now meet -- but which face touches which, and in what
// order, is exactly as it was.
//
// So there is one routine underneath, RebuildOnSurfaces, and the three
// operations are three ways of deciding what the new surfaces should be.
// That is worth stating because the alternative, which is what a modeller
// reaches for when it has a boolean and is pleased with it, is to build
// the offset body separately and subtract. Part E.3 records at length
// what happens when a local edit is done as a boolean: the two bodies
// touch along whole faces, which is precisely the case the boolean does
// not handle.
//
// WHAT IS HANDLED, precisely: bodies whose every face is a plane, and
// whose every vertex has exactly three faces meeting at it. That is
// every prism, every polyhedron, and everything Parts E.1 and E.2 build
// from a polygonal profile. Curved faces are refused by name, because a
// curved face needs the new edge curves to come from intersecting the
// new surfaces rather than from joining the new corners -- a table of
// surface-pair cases that belongs with Part C's intersector and is not a
// tolerance away from this.
//
// SELF-INTERSECTION IS REPORTED, NOT RESOLVED. Offset a box inwards by
// more than half its thickness and there is no answer to give: the walls
// pass through each other. The test for it is cheap and exact -- an edge
// whose direction reversed is an edge whose two ends swapped over, which
// is what a feature being consumed looks like from here -- and a
// refusal naming that edge is worth far more than a body that validates
// and is wrong.
namespace cad {

struct ModifyOptions {
    Tolerance tolerance;
};

// One face's replacement surface. The surface's parameter domain is
// ignored and recomputed from where the face's corners end up, so a
// caller building one of these need only get its position right.
struct FaceSurface {
    EntityId face = kNoEntity;
    std::shared_ptr<const Surface> surface;
};

// Re-solves `body` against replacement surfaces, keeping its topology.
// Faces not named in `replacements` keep the surface they had.
bool RebuildOnSurfaces(const Model &model, EntityId body, const std::vector<FaceSurface> &replacements,
                       const ModifyOptions &options, Model *out, EntityId *out_body, std::string *error);

// Moves every face along its own outward normal. A positive distance
// grows the body, a negative one shrinks it.
bool OffsetBody(const Model &model, EntityId body, double distance, const ModifyOptions &options, Model *out,
                EntityId *out_body, std::string *error);

// Tilts each named face by `angle` about the line where it meets the
// neutral plane, which is what a moulded part needs in order to come out
// of its mould. A positive angle leans the face so that the body narrows
// in the direction the neutral plane's normal points -- the pull
// direction -- which is the sign convention every mould tool uses and
// the only one that makes "two degrees of draft" mean one thing.
bool DraftFaces(const Model &model, EntityId body, const std::vector<EntityId> &faces,
                const Vec3d &neutral_point, const Vec3d &pull_direction, double angle,
                const ModifyOptions &options, Model *out, EntityId *out_body, std::string *error);

// Hollows the body out to a wall of `thickness`, opening it at each face
// in `open_faces`.
//
// WITH NO FACES OPENED this is thicken: a closed cavity inside the body,
// carried as an inner shell, which is what a sealed hollow part is.
//
// WITH FACES OPENED the cavity reaches the surface there, and what is
// left on that face is a rim of exactly `thickness` -- the face with the
// cavity's mouth as a hole in it. Getting that right is the whole
// difference between shelling a box and merely putting a smaller box
// inside it: the cavity is *not* the body offset inwards, because it
// must run all the way out to the opened face rather than stopping a
// wall's thickness short of it.
bool ShellBody(const Model &model, EntityId body, const std::vector<EntityId> &open_faces, double thickness,
               const ModifyOptions &options, Model *out, EntityId *out_body, std::string *error);

}  // namespace cad

#endif
