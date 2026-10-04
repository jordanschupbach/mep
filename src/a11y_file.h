#pragma once
// Any document mep can read, as a screen reader meets it (a11y_doc.h),
// chosen by the file's extension: a .pdf from its tags (a11y_pdf.h), an
// .html page from its DOM (a11y_html.h), a .mepml document from its parse
// (mepml_a11y.h), and what mepml imports -- .docx, .odt, .rtf, .md, .org
// -- by way of that import, which keeps their pictures' descriptions and
// their tables' header rows.

#include "a11y_doc.h"

#include <string>

namespace a11y {

// False with *error when the file cannot be read or is of no known kind.
bool FromFile(const std::string &path, Document *out, std::string *error);
// The same from bytes already in hand (a PDF open in the editor).
bool FromPdfBytes(const unsigned char *data, size_t len, Document *out, std::string *error);

}  // namespace a11y
