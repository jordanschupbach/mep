#ifndef MEP_OFFICE_MATH_H
#define MEP_OFFICE_MATH_H

// Office documents' maths read back as TeX: Word's and PowerPoint's OMML
// (<m:oMath>, <m:oMathPara>) and OpenDocument's MathML (a formula object's
// <math>). Both describe maths as structure; mep writes and typesets TeX.
// A MathML <semantics> with a TeX annotation (what mep's own exports
// write) gives that TeX back as it was.

#include <string>

#include "xml_doc.h"

namespace officemath {

std::string OmmlToTex(const xml::xml_node &n);
std::string MathmlToTex(const xml::xml_node &n);

}  // namespace officemath

#endif
