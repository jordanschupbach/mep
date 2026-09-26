#ifndef MEP_CAD_DOC_H
#define MEP_CAD_DOC_H

#include "cad_feature.h"
#include "cad_sketch.h"

#include <string>

// The native document (plans/CAD_FEM_PLAN.md Part F.1).
//
// `.mepcad` is the only lossless format. Everything else mep reads or
// writes is an export of the *evaluated* body -- a STEP file carries the
// faces a feature tree produced but not the tree, so opening one and
// changing the extrude distance is not a thing that can be done. What
// makes this format the exception is that it stores the operations
// rather than their result: the sketches, the features, their numbers,
// and the persistent names their references are recorded under.
//
// WHAT IS NOT STORED IS THE POINT. There is no geometry in a `.mepcad`
// file -- no faces, no edges, no surfaces. Reading one gives back a
// FeatureTree that has not been evaluated; calling Rebuild on it
// reproduces the body. That is a real claim about the kernel rather than
// a storage decision, and the test makes it the way it has to be made:
// build a part, save it, load it, rebuild, and compare the volume and the
// topology counts against the original.
//
// VERSIONED FROM DAY ONE, because the alternative is discovering on the
// day of the first change that there is no way to tell an old file from a
// new one. The version is the first thing written and the first thing
// checked, and a file from the future is refused by name rather than
// half-read.
namespace cad {

// Bumped whenever the meaning of anything below changes. A reader
// accepts its own version and every older one it still knows how to
// read; it refuses anything newer.
inline constexpr int kCadDocumentVersion = 1;

struct CadDocument {
    // Free-form, for the application: a part number, a title block, a
    // note. Stored verbatim and never interpreted here.
    std::string title;
    std::string notes;
    FeatureTree tree;
};

// Serialises to text. Never fails: a document is a value, and everything
// in it is representable.
std::string WriteCadDocument(const CadDocument &document);

// Reads it back. Returns false with a message naming what was wrong --
// the version, a missing field, an unknown kind -- rather than producing
// a partly-filled document.
bool ReadCadDocument(const std::string &text, CadDocument *out, std::string *error);

// The version a file claims, without reading the rest of it. Returns
// false if it is not a `.mepcad` document at all.
bool CadDocumentVersion(const std::string &text, int *out_version, std::string *error);

// Sketches are written and read on their own as well, because they are
// worth exchanging on their own -- and because a sketch is the part of
// this that has ids referring to other ids, so testing it separately is
// what makes a failure in the whole document easy to place.
std::string WriteSketch(const Sketch &sketch);
bool ReadSketch(const std::string &text, Sketch *out, std::string *error);

}  // namespace cad

#endif
