#pragma once
// mep's own reader for a PDF's logical structure (spec 14.7) -- what makes
// a PDF a *tagged* one (14.8), readable by a screen reader: the tree under
// the catalog's /StructTreeRoot says what every piece of every page is (a
// heading, a paragraph, a table's header cell, a figure) and carries the
// text that stands for what cannot be read as text (a figure's /Alt, a
// formula's). Like pdf_outline.h, successor work to PDFIUM_REMOVAL_PLAN.md:
// built on pdf_object.h / pdf_xref.h / pdf_document.h, raylib-free.
//
// An element's content is named, not held: a kid of an element is another
// element, or a marked-content sequence of some page (its /MCID -- the
// content stream marks the glyphs, see pdf_content.h's TextGlyph::mcid), or
// a whole object (an annotation, an XObject). a11y_pdf.h puts the two
// halves together into the document as it is read.

#include "pdf_document.h"
#include "pdf_object.h"
#include "pdf_xref.h"

#include <cstddef>
#include <string>
#include <vector>

namespace pdfstruct {

struct Kid {
    enum class Kind { Element, Content, Object };
    Kind kind = Kind::Element;
    int element = -1;  // Element: index into Tree::elements
    int page = -1;     // Content / Object: 0-based page index, -1 if it names no page of the document
    int mcid = -1;     // Content: the marked-content identifier on that page
};

struct Element {
    std::string type;  // /S as the file writes it ("H1", "Figure", or a name of the producer's own)
    // The standard structure type it stands for (spec 14.8.4): `type`
    // itself, or what the tree's /RoleMap maps it to.
    std::string role;
    // Text strings, as UTF-8; has_alt tells an empty /Alt from none.
    std::string alt, actual_text, title, lang, expansion, id;
    bool has_alt = false;
    // From the element's attribute objects (/A, spec 14.8.5): a table's
    // /Summary, a header cell's /Scope ("Column", "Row", "Both"), a list's
    // /ListNumbering.
    std::string summary, scope, list_numbering;
    int page = -1;  // its /Pg: the page its integer kids are on
    int parent = -1;
    std::vector<Kid> kids;
};

struct Tree {
    bool present = false;  // the catalog has a /StructTreeRoot
    bool marked = false;   // /MarkInfo << /Marked true >>: the file says it is tagged
    std::string lang;      // the catalog's /Lang
    std::string title;     // the document information's /Title
    bool display_title = false;  // /ViewerPreferences << /DisplayDocTitle true >>
    std::vector<Element> elements;
    std::vector<int> roots;  // the elements straight under the root, in order
    // Whether the root has a /ParentTree (spec 14.7.4.4): what a reader
    // going from a page's content to its element needs.
    bool parent_tree = false;
    // That tree, for the pages: page_parents[p][mcid] is the element
    // (index into `elements`) the marked-content sequence `mcid` of page
    // `p` belongs to, -1 where the tree has no element for it. Empty for a
    // page with no /StructParents or no entry in the tree.
    std::vector<std::vector<int>> page_parents;
    // True when every marked-content kid of every element is found in
    // page_parents under that element: the two directions agree.
    bool ParentsConsistent() const;
};

// Reads the catalog's accessibility entries and the whole structure tree.
// Tolerant like the rest of this engine: a kid that cannot be resolved is
// skipped, a cycle is cut, and a document with no tree at all gives a Tree
// with `present` false and its title and language still filled in.
// `data`/`len` must be the buffer `table` was loaded from.
Tree Read(const unsigned char *data, size_t len, const pdfxref::XrefTable &table, const pdfdoc::PdfDocument &document);

}  // namespace pdfstruct
