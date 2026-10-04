#ifndef MEP_A11Y_DOC_H
#define MEP_A11Y_DOC_H

#include <string>
#include <vector>

// A document as a screen reader meets it: not its pages or its markup but
// what each piece *is* -- a heading and its level, a paragraph, a table and
// which of its cells head a column, a figure and the text that stands for
// it, a formula and how it is read aloud. One model for every format mep
// reads or writes, so the same three questions can be asked of all of them:
//
//   Read     how the document reads, line by line, to someone who cannot
//            see it (figures as their alt text, tables cell by cell under
//            their headers, decoration left out)
//   Outline  its structure, as a tree
//   Check    what is missing for such a reader (a figure with no alt text,
//            a table with no header row, no title, no language ...)
//
// Each format builds the model its own way: mepml from its parse
// (mepml_a11y.h), a PDF from its structure tree and marked content
// (a11y_pdf.h), HTML from its DOM (a11y_html.h). Deliberately free of all
// of them, and of raylib: plain data and pure functions.

namespace a11y {

enum class Role {
    Document,
    Section,     // a part of the document: a slide, a titled box, an abstract
    Title,       // the document's own title, where the document shows it
    Heading,     // `level` 1..6
    Paragraph,
    Text,        // a run of text: `text`; no children
    List,
    ListItem,
    Label,       // a list item's bullet or number
    Table,
    Row,
    HeaderCell,  // `scope`: "column" or "row"
    Cell,
    Figure,
    Formula,
    Caption,
    Code,
    Quote,
    Note,        // a callout, a footnote
    Link,
    Group,       // holds its children and says nothing itself
};

struct Node {
    Role role = Role::Group;
    std::string tag;   // what the format calls it ("H2", "img", "image")
    int level = 0;     // Heading
    std::string text;  // Text
    // The text that stands for the node (a figure's, a formula's).
    // `has_alt` tells an empty one from none; an empty one on a figure
    // says it only decorates (`decorative`), and it is then not read.
    std::string alt;
    bool has_alt = false;
    bool decorative = false;
    std::string summary;  // Table: what it shows, for a reader who cannot see it
    std::string scope;    // HeaderCell
    std::string lang;     // where it differs from the document's
    // Where it is: a 0-based page and a box on it in points (left, bottom,
    // right, top; y up) for a paged format, a 0-based source line for a
    // text one. -1 / no box when unknown.
    int page = -1;
    int line = -1;
    bool has_box = false;
    double box[4] = {0, 0, 0, 0};
    std::vector<Node> children;
};

struct Document {
    std::string format;  // "mepml", "pdf", "html"
    std::string title;
    std::string lang;    // a BCP 47 tag, "" when the document names none
    // False for a document with no structure of its own to read (a PDF
    // that is not tagged): the tree is then only its text, in page order.
    bool tagged = true;
    // Text runs carry their own spacing (true for markup; a PDF's are
    // glyphs, joined with a space between runs).
    bool spaced = true;
    Node root;
};

const char *RoleName(Role role);

// A subtree as one line of text: figures and formulas as their alt text,
// decoration left out.
std::string InlineText(const Document &doc, const Node &node);

struct Line {
    std::string text;
    int depth = 0;  // indent, in steps
    int page = -1;  // of the node the line is for
    int line = -1;
};
// The document as it reads to someone who cannot see it.
std::vector<Line> Read(const Document &doc);
// Its structure: one line per node, text cut to `text_cols` columns.
std::vector<Line> Outline(const Document &doc, int text_cols = 60);

enum class Severity { Error, Warning, Info };
struct Issue {
    Severity severity = Severity::Warning;
    std::string code;     // "figure-alt", "table-header", "doc-title" ...
    std::string message;
    int page = -1;
    int line = -1;
};
// What a reader who cannot see the document is missing, most serious
// kind first within the document's order.
std::vector<Issue> Check(const Document &doc);
const char *SeverityName(Severity s);

// Read / Outline / Check as plain text (`what`: "read", "tree", "check"),
// lines indented two spaces a step, each issue as "error: message (page 3)".
std::string Report(const Document &doc, const std::string &what);

}  // namespace a11y

#endif  // MEP_A11Y_DOC_H
