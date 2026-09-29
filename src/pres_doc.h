#ifndef MEP_PRES_DOC_H
#define MEP_PRES_DOC_H

// A presentation (slide deck) as mep's presentation editor holds it, and
// its two file formats: PowerPoint (.pptx) and OpenDocument / LibreOffice
// Impress (.odp) -- read and written by src/pres_pptx.cpp and
// src/pres_odp.cpp. Pure data and file I/O; no window (the editor's
// PresSession and DrawPresPane draw and edit it).
//
// The model is what a slide is made of once a program's own machinery --
// masters, layouts, placeholders, inherited styles -- has been applied:
// every shape has its own position, size, fill and text styling. Reading
// resolves what the file inherits; writing spells everything out on the
// slide. So a deck opened and saved again looks the same but is flatter
// than it was (its placeholders become plain text boxes), and what the
// model has no field for (animations, transitions, charts, SmartArt,
// speaker notes' formatting) does not survive.
//
// Lengths are EMU (English Metric Units: 914400 to the inch, 12700 to the
// point), PowerPoint's own unit; .odp's centimetres convert on the way.

#include <functional>
#include <memory>
#include <string>
#include <vector>

namespace pres {

constexpr long kEmuPerInch = 914400;
constexpr long kEmuPerPt = 12700;

struct TextRun {
    std::string text;
    bool bold = false, italic = false, underline = false, strike = false;
    double size_pt = 0;  // 0: the shape's default (Shape::font_pt)
    std::string color;   // RRGGBB, "" for the shape's default text colour
    std::string font;    // "" for the deck's default face
    std::string link;    // a hyperlink's target, "" for none
    int baseline = 0;    // +1 superscript, -1 subscript
    // Maths: its TeX, typeset in place of `text` -- which is then the one
    // object character kMathChar, so the caret and Backspace treat an
    // equation as one character. `display` sets it as an equation of its
    // own (big fractions, limits over sums) rather than inline.
    std::string tex;
    bool display = false;
    bool IsMath() const { return !tex.empty(); }
};

// U+FFFC OBJECT REPLACEMENT CHARACTER: a maths run's text.
inline constexpr const char *kMathChar = "\xEF\xBF\xBC";
TextRun MathRun(const std::string &tex, bool display);
// A shape that is one displayed equation (and nothing else).
bool IsEquationShape(const struct Shape &s);

enum class Align { Left, Center, Right, Justify };

struct Paragraph {
    std::vector<TextRun> runs;
    Align align = Align::Left;
    int level = 0;        // list depth (0 = top)
    bool bullet = false;  // a bulleted list item
    bool numbered = false;
    std::string PlainText() const;
};

enum class ShapeKind { Text, Rect, RoundRect, Ellipse, Line, Image };
enum class VAlign { Top, Middle, Bottom };

struct Shape {
    ShapeKind kind = ShapeKind::Text;
    long x = 0, y = 0, w = 0, h = 0;
    std::string name;
    std::string fill;        // RRGGBB, "" for none
    std::string line;        // RRGGBB, "" for none
    double line_pt = 0;      // outline width
    // Text: in any kind but Image and Line.
    std::vector<Paragraph> paras;
    double font_pt = 18;          // the default size of its runs
    std::string text_color;       // RRGGBB, "" for black
    VAlign valign = VAlign::Top;
    bool is_title = false;        // the slide's title (a title placeholder when written)
    bool autofit = false;         // shrink text to fit (normAutofit / shrink-to-fit)
    // Image: the encoded picture (PNG, JPEG, ...), shared between the
    // copies undo keeps.
    std::shared_ptr<const std::string> image;
    std::string image_ext;        // "png", "jpeg", "gif", ...
    std::string alt;              // an image's description
    bool HasText() const;
    std::string PlainText() const;  // paragraphs joined by '\n'
};

struct Slide {
    std::vector<Shape> shapes;  // back to front
    std::string background;     // RRGGBB, "" for white
    std::string notes;          // speaker notes, plain text
};

struct Presentation {
    long width = 12192000, height = 6858000;  // 16:9, 13.333 x 7.5 in
    std::vector<Slide> slides;
    std::string title, author;
};

// A new deck: one title slide with a title and a subtitle box.
Presentation NewPresentation();
// A blank slide shaped for `p` with a title and a body box (the layout
// "title and content"), or with nothing on it.
Slide NewSlide(const Presentation &p, bool with_boxes = true);
// A text box / shape placed in the middle of the slide.
Shape NewTextBox(const Presentation &p, const std::string &text);
Shape NewShape(const Presentation &p, ShapeKind kind);
// A displayed equation, as large as it is at `pt`, in the middle of the slide.
Shape NewEquation(const Presentation &p, const std::string &tex, double pt = 32);
// Inline maths at the caret (text editing, below).
void InsertMath(Shape &s, int *para, int *off, const std::string &tex);

// Text editing, as the editor types: a caret is a paragraph and a byte
// offset into its plain text (Paragraph::PlainText). Inserted text takes
// the style of the run it lands in (the earlier one at a boundary); an
// empty paragraph takes the look of the text before it.
void InsertText(Shape &s, int *para, int *off, const std::string &text);
// Deletes the character before (or after) the caret, joining paragraphs at
// either end of one. False when there was nothing to delete.
bool DeleteChar(Shape &s, int *para, int *off, bool forward);
// Enter: the paragraph splits at the caret; the new one keeps its list
// and alignment.
void SplitParagraph(Shape &s, int *para, int *off);

// A picture of an equation for a .pptx's fallback (what a reader without
// Office equations shows -- LibreOffice Impress is one): PNG bytes and the
// size it is set at, in points. The editor sets one (its typesetter);
// without one the fallback is the equation as text.
using MathPictureFn = std::function<bool(const std::string &tex, double pt, std::string *png, double *w_pt, double *h_pt)>;
void SetMathPictureRenderer(MathPictureFn fn);

// By extension (.pptx or .odp). False with *error on failure.
bool Load(const std::string &path, Presentation *out, std::string *error);
bool Save(const Presentation &p, const std::string &path, std::string *error);
bool IsPresentationPath(const std::string &path);

bool LoadPptx(const std::string &bytes, Presentation *out, std::string *error);
std::string SavePptx(const Presentation &p);
bool LoadOdp(const std::string &bytes, Presentation *out, std::string *error);
std::string SaveOdp(const Presentation &p);

// Helpers the readers and writers share.
// "#1f2328" / "1F2328" -> "1F2328"; "" when it is not a colour.
std::string NormalizeColor(const std::string &c);
// An ODF length ("2.54cm", "1in", "72pt", "25.4mm") in EMU.
long OdfLengthToEmu(const std::string &len);
std::string EmuToCm(long emu);

}  // namespace pres

#endif
