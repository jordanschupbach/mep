#pragma once

// mep's own in-house document-wide text search -- see
// PDFIUM_REMOVAL_PLAN.md Phase 11. Builds on pdf_content.h's
// ExtractContentStreamText (Phase 11's other half, living alongside the
// content-stream interpreter since it reuses that exact same class) to
// turn a page's glyph-by-glyph Unicode text into a searchable per-page
// string, then reproduces pdf_doc.h's PdfDoc::Search/MatchRectsForPage
// contract exactly -- same struct field names/order in
// PdfTextRectPt/PdfTextMatch/PdfHighlightRect (just namespaced here to
// avoid colliding with pdf_doc.h's own globals while both exist side by
// side pre-Phase-13) -- so Phase 13's eventual swap-in is a mechanical
// rename, not a rewrite.
//
// Scoping decision -- word/line-break reconstruction: a PDF content
// stream carries no explicit word- or line-break markers (a "new line"
// is just the next Tj/TJ run positioned lower on the page via its own
// Td/TD/Tm, and inter-word spacing is often just a wider glyph-position
// gap, not always a literal space character). This module infers both
// from glyph geometry alone: consecutive glyphs whose vertical centers
// differ by more than half either one's height start a new line (also
// where PdfTextMatch's own rects split -- matching FPDFText_CountRects'
// "merges same-line character boxes" behavior, which by construction
// never merges across a line); consecutive same-line glyphs separated by
// a horizontal gap wider than a quarter of either one's height get a
// synthetic space inserted so a multi-word query can still find them.
// Approximate by nature (a real space character already existing at that
// same spot naturally reproduces the same behavior via the geometry
// check, so this rarely double-inserts); verified against pdftotext's
// own extraction across every Phase 1 fixture, not expected to be
// byte-exact.

#include "pdf_document.h"
#include "pdf_xref.h"

#include <string>
#include <vector>

namespace pdftext {

// One text-search match's highlight rect, in PDF-point space -- see
// pdf_doc.h's own PdfTextRectPt doc comment for the exact convention
// (page-native units, scale-/rotation-independent; top/bottom use PDF's
// own y-up sense, so top >= bottom).
struct PdfTextRectPt {
    double left = 0, top = 0, right = 0, bottom = 0;
};

struct PdfTextMatch {
    int page = 0;
    std::vector<PdfTextRectPt> rects_pt;
    // A snippet of the page's text around the match, for a result list,
    // and the match's byte range in it ([context_hl_start, context_hl_end)).
    std::string context;
    int context_hl_start = 0, context_hl_end = 0;
};

struct PdfHighlightRect {
    int match_index = 0;
    float x0 = 0, y0 = 0, x1 = 0, y1 = 0;
};

// Plain concatenated text for one page, in reading order -- exposed
// mainly for verification (diffing against `pdftotext`'s own extraction
// of the same fixture); Search below builds and discards the same text
// internally per page, it doesn't call this.
std::string ExtractPageText(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                             const pdfdoc::Page &page);

// Case-insensitive (ASCII-fold) substring search across every page of
// `doc`, in page order, non-overlapping matches per page (matches
// PDFium's own FPDFText_FindNext convention of advancing past each
// found match). `doc_data`/`doc_len` must be the same buffer `doc` was
// loaded from.
std::vector<PdfTextMatch> Search(const unsigned char *doc_data, size_t doc_len, const pdfdoc::PdfDocument &doc,
                                  const std::string &query);

// Every page's searchable text, extracted once so a search per keystroke
// does not re-run each page's content stream (BuildSearchIndex, then the
// Search overload below -- the same matches as the one above).
struct SearchPage {
    std::string text, lower;           // `lower`: ASCII-folded, for matching
    std::vector<int> char_glyph;       // per byte of `text`: its glyph, or -1 for a synthetic separator
    std::vector<bool> glyph_new_line;  // per glyph: starts a new line
    struct Box {
        float left, top, right, bottom;
    };
    std::vector<Box> glyph_boxes;  // per glyph, PDF points
};
struct SearchIndex {
    std::vector<SearchPage> pages;
};
SearchIndex BuildSearchIndex(const unsigned char *doc_data, size_t doc_len, const pdfdoc::PdfDocument &doc);
// `context_limit`: how many of the first matches get a `context` snippet
// (a result list shows a bounded number; building one per match of a
// one-letter query in a long document is not free).
std::vector<PdfTextMatch> Search(const SearchIndex &index, const std::string &query, size_t context_limit = 0);

// Converts every rect (across all of `matches`) belonging to page_index
// into device pixels at px_per_pt, via the same PageToDeviceMatrix
// RenderPage itself would use -- mirrors PdfDoc::MatchRectsForPage.
std::vector<PdfHighlightRect> MatchRectsForPage(const pdfdoc::PdfDocument &doc, int page_index, float px_per_pt,
                                                 const std::vector<PdfTextMatch> &matches);

// One extracted glyph's bounding box in PDF-point space (y-up, top >=
// bottom), in reading order -- the geometry the viewer point-hit-tests
// for click-drag text selection. Same convention as PdfTextRectPt.
//
// `text` is what the glyph reads as (more than one character for a
// ligature, whose /ToUnicode is e.g. "fi"), and `actual_text` is the
// /ActualText of the marked-content sequence it was shown in, which
// overrides it when present (spec 14.9.4). Both are what lets a
// selection be copied and quoted rather than only drawn around -- see
// JoinGlyphText.
struct GlyphBox {
    double left = 0, top = 0, right = 0, bottom = 0;
    std::string text;
    std::string actual_text;
    // Whether this glyph is page furniture rather than content: inside
    // an /Artifact marked-content sequence (spec 14.8.2.2), which is how
    // a tagged PDF labels its own running heads, folios and rules.
    //
    // pdfrender::TextGlyph has carried this all along; it stopped here,
    // so the selection and AI paths were structure-blind while the
    // accessibility path (a11y_pdf.cpp) already filtered on it. A reader
    // dragging over two paragraphs has no use for the running head that
    // happens to sit between them in the file.
    bool artifact = false;
};

// Every glyph box on `page`, in reading order (point space) -- the raw
// material for text selection. `doc_data`/`doc_len` must be the buffer
// `table` was loaded from.
std::vector<GlyphBox> PageGlyphBoxes(const unsigned char *doc_data, size_t doc_len, const pdfxref::XrefTable &table,
                                     const pdfdoc::Page &page);

// A ligature glyph (U+FB00..FB04: ff fi fl ffi ffl) rewritten as the
// letters it stands for -- what the word actually is, which is what a
// reader copying a passage out wants rather than the typographic form.
std::string ExpandLigatures(const std::string &s);

// Whether `s` ends in a letter followed by a hyphen, i.e. a word broken
// over a line end and not a real hyphenated compound.
bool EndsWithLetterHyphen(const std::string &s);

// The glyphs at `indices` (positions in `glyphs`, in the order given)
// as readable text: a space where the next glyph starts a new word or a
// new line, a newline between lines, a word hyphenated over a line end
// put back together, ligatures expanded, and an /ActualText sequence
// read as its own text once rather than per glyph.
//
// Uses the same geometry rules as the rest of this header -- a new line
// when vertical centres differ by more than half a glyph height, a word
// gap at more than a quarter of one -- so what is copied matches what
// search and selection already agree a line and a word are.
std::string JoinGlyphText(const std::vector<GlyphBox> &glyphs, const std::vector<int> &indices);

// One visual row as one line of text: word gaps from the horizontal
// geometry, and no line breaks at all, because a row is one line of
// type however many baselines its glyphs sit on.
//
// This is the piece JoinGlyphText cannot get right on its own. Handed a
// bare run of indices, it has to infer line breaks from vertical steps,
// and inside a formula every step between baselines looks like one --
// which extracted a real thesis page's limit as "\u2206 / li / t / m /
// \u21920" instead of "lim \u2206t\u21920". A caller that knows where
// the rows are should not pay for that guess.
std::string JoinGlyphRowText(const std::vector<GlyphBox> &glyphs, const std::vector<int> &indices,
                             int rotate = 0);

// Whole visual rows as readable text: each row joined by
// JoinGlyphRowText, exactly one newline between rows, and a word broken
// over a row end put back together. What PageText and a multi-row
// selection want.
std::string JoinGlyphRows(const std::vector<GlyphBox> &glyphs, const std::vector<std::vector<int>> &rows,
                          int rotate = 0);

// One visual line of a page's glyphs -- what a keyboard caret steps
// between, as opposed to the raw reading order PageGlyphBoxes returns.
//
// The two differ more than you would hope. A content stream's order is
// whatever the producer emitted, which for a typeset book is routinely
// body text, then the running header, then the figure caption, then the
// footnotes (451 of the 485 pages of the Causality.pdf fixture jump
// back up the page at least once); and a line's own glyphs arrive
// interleaved with their subscripts, superscripts and big operators,
// which sit on their own baselines. A caret walking that order visits a
// displayed equation's summation signs, their indices and the main
// baseline in turn, bouncing up and down by a few points at a time, and
// then teleports to the top of the page.
//
// So: glyphs are grouped onto the baseline they belong to, smaller
// satellites (sub/superscripts, the arms of a fraction) are folded into
// the nearest baseline they could be attached to, and the rows come
// back top to bottom with each row left to right.
struct GlyphRow {
    std::vector<int> glyphs;  // indices into the PageGlyphBoxes result, left to right
    double top = 0, bottom = 0;
    // A dedicated equation line -- a row that is set apart from the text
    // block and whose glyphs sit on several baselines, i.e. the ones
    // whose internal structure is what makes caret motion erratic in the
    // first place. Running text with an inline subscript is NOT this: one
    // dominant baseline with a stray glyph off it stays ordinary text.
    //
    // Purely informational: these rows are navigable and selectable like
    // any other. The caret used to skip them, which made a displayed
    // equation impossible to select or yank at all; a row's glyphs are
    // now ordered so that walking one is monotonic (see the ordering
    // note above), so there is nothing to skip. The flag remains for a
    // caller that wants to treat an equation differently.
    bool display_math = false;
};

// Groups a page's glyph boxes into visual lines. See GlyphRow. Every
// glyph index appears in exactly one row, so a caller that ignores
// `display_math` loses nothing.
// `rotate` is the page's own /Rotate (0/90/180/270, already normalized
// by pdf_document.h). It matters because glyph boxes are in the page's
// unrotated point space -- which they must stay in, since annotation
// quads round-trip through it -- so on a landscape page the axis text
// advances along is the one this grouper would otherwise take for the
// line-advance axis, and the page comes back one character per line.
// Defaults to 0 so a caller measuring an upright page need not say so.
std::vector<GlyphRow> PageGlyphRows(const std::vector<GlyphBox> &glyphs, int rotate = 0);

// The glyph indices a click-drag between two point-space positions
// covers: everything between the glyph nearest the anchor and the one
// nearest the head, in VISUAL order -- the same order the keyboard
// caret moves in, so a drag and a keyboard selection cannot disagree
// about what is selected. (This used to be the contiguous run in
// content-stream order, which on a page that emits its running header
// after the body put the header inside a selection of two body
// paragraphs.) What SelectionRects draws
// around, and what JoinGlyphText turns into copyable text -- shared so
// the two can never disagree about what is selected.
std::vector<int> SelectionGlyphRange(const std::vector<GlyphBox> &glyphs, double ax, double ay, double bx, double by,
                                     int rotate = 0);

// Given a page's glyph boxes (reading order) and two point-space points
// (a selection's anchor and head, in either order), returns the selection
// as per-line rects (point space): the union of each run of same-line
// glyphs between the glyph nearest the anchor and the glyph nearest the
// head. Uses the same vertical-center line-break heuristic as Search.
// Empty if `glyphs` is empty.
std::vector<PdfTextRectPt> SelectionRects(const std::vector<GlyphBox> &glyphs, double ax, double ay, double bx,
                                          double by, int rotate = 0);

}  // namespace pdftext
