// mep's mepml formatter: what `gf` does to a .mepml document outside its
// code blocks (the blocks themselves go to their own language's formatter,
// kBuiltinFormat in main.cpp).
//
// What it changes:
//   - paragraphs, list items and an \abstract's paragraphs are refilled to
//     `width` columns (a list item's later lines hang under its text);
//   - a comment or callout with a line past `width` is refilled (one that
//     fits is left as written -- a comment may be laid out on purpose);
//   - table columns are padded so the pipes line up, the separator row
//     sized to match (its alignment colons kept);
//   - trailing whitespace goes, runs of blank lines become one, and blank
//     lines at the top and bottom of the file go.
//
// What it never touches: code blocks (fences, options, body and results),
// display maths, \define / \raw / user-command blocks, HTML and SVG markup,
// headings' text, and anything inside a results=markdown region (that is a
// program's output).
//
// Refilling never splits a construct that has to stay on one line --
// maths, `verbatim`, links, citations, a command's arguments -- nor starts
// a line with something that would read as a different block (`- `, `1.`,
// `> `, `//` ...). An inline `// comment` stays at the end of its line.
//
// And it checks itself: the result is parsed again and must be the same
// document as before -- the same blocks with the same content, whitespace
// aside. A change that would alter it is dropped and counted in
// FormatDocumentResult::kept, so formatting can make a file tidier but
// never make it mean something else.
#ifndef MEP_MEPML_FORMAT_H
#define MEP_MEPML_FORMAT_H

#include <string>

#include "mepml_doc.h"

namespace mepml {

struct FormatDocumentOptions {
    int width = 80;  // columns, counted in characters (not bytes)
};

struct FormatDocumentResult {
    bool ok = true;
    std::string text;  // the formatted document, ending in a newline
    std::string error;
    int error_line = 0;
    // Changes left out because the document would have parsed differently
    // with them in (see the file comment).
    int kept = 0;
};

FormatDocumentResult FormatDocument(const std::string &text, const FormatDocumentOptions &opt = {});

// What the self-check compares: everything a parse says about the
// document except where it sits and how its prose is spaced.
std::string DocumentSignature(const Document &doc);

}  // namespace mepml

#endif  // MEP_MEPML_FORMAT_H
