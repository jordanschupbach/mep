#ifndef MEP_MEPML_A11Y_H
#define MEP_MEPML_A11Y_H

#include "a11y_doc.h"
#include "mepml_doc.h"

// A mepml document as a screen reader would meet any export of it
// (a11y_doc.h): its headings, paragraphs, lists and tables, its figures as
// their \alttext, its formulas as theirs. What the language has for such a
// reader:
//
//   //? Title: ...          the document's title
//   //? Lang: en-GB         its language (DocumentLanguage)
//   \alttext(text)          under a figure (\image, a code block's plot):
//                           the text read in its place; under display maths
//                           or after inline maths: how it is read aloud;
//                           under a table: what the table shows
//   \alttext()              under a figure: it only decorates, and is left
//                           out of what is read
//   a table's header row    (the rows above |---|) heads its columns
//   \caption(text)          a figure's, a table's, a formula's caption
//
// a11y::Check over the result is the language's accessibility lint (the
// language server reports it, `mep-mepml a11y FILE check` prints it), and
// every node knows its source line.

namespace mepml {

a11y::Document Accessibility(const Document &doc);

}  // namespace mepml

#endif  // MEP_MEPML_A11Y_H
