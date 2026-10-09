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
    // The caller decides what to do with the flag; a caret skips these.
    bool display_math = false;
};

// Groups a page's glyph boxes into visual lines. See GlyphRow. Every
// glyph index appears in exactly one row, so a caller that ignores
// `display_math` loses nothing.
std::vector<GlyphRow> PageGlyphRows(const std::vector<GlyphBox> &glyphs);

// The glyph indices a click-drag between two point-space positions
// covers: the contiguous run between the glyph nearest the anchor and
// the one nearest the head, in reading order. What SelectionRects draws
// around, and what JoinGlyphText turns into copyable text -- shared so
// the two can never disagree about what is selected.
std::vector<int> SelectionGlyphRange(const std::vector<GlyphBox> &glyphs, double ax, double ay, double bx, double by);

// Given a page's glyph boxes (reading order) and two point-space points
// (a selection's anchor and head, in either order), returns the selection
// as per-line rects (point space): the union of each run of same-line
// glyphs between the glyph nearest the anchor and the glyph nearest the
// head. Uses the same vertical-center line-break heuristic as Search.
// Empty if `glyphs` is empty.
std::vector<PdfTextRectPt> SelectionRects(const std::vector<GlyphBox> &glyphs, double ax, double ay, double bx,
                                          double by);

}  // namespace pdftext
