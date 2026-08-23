#ifndef MEP_OFFICE_DOC_H
#define MEP_OFFICE_DOC_H

#include <cstddef>
#include <functional>
#include <string>
#include <utility>
#include <vector>

// Deliberately raylib-free (same reasoning as image_doc.h/pdf_doc.h): the
// document model, parsing, and span-editing logic are pure CPU-side data
// structures, usable/testable without a GL context. main.cpp is the only
// place that turns a paragraph's spans into rendered/word-wrapped text.
//
// A hand-rolled, intentionally partial rich-text model for .docx/.odt --
// not a general-purpose office-document library. Tables round-trip as
// plain-text cells (no per-cell formatting/merged cells/nested tables);
// images render inline and insert, but don't yet round-trip through save
// (DocImage isn't written back to word/media//Pictures -- see
// SaveDocxToMemory/SaveOdtToMemory). No headers/footers, footnotes/
// comments, track changes, real numbered lists (bullet-or-not only), or
// full OOXML/ODF style-cascade inheritance. Legacy binary .doc is out of
// scope entirely. See WYSIWYG_TOOLBAR_PLAN.md and NVIM_PARITY_PLAN.md's
// Office pane phase for the full exclusion list and rationale.

// Logical font family a run draws with -- Sans is the pre-existing (and
// still default) Liberation Sans; Serif/Mono are Liberation Serif/Mono,
// same OFL-licensed family, embedded the same way (office_font_data_serif.h/
// office_font_data_mono.h). All three are real, distinct glyph atlases, not
// just a stored label -- a font-family *picker* genuinely changes what's
// drawn, unlike a v1 that would only round-trip the name.
enum class OfficeFontFamily { Sans, Serif, Mono };

struct DocFormat {
    bool bold = false, italic = false, underline = false, strike = false;
    bool superscript = false, subscript = false;  // mutually exclusive in practice; toggling one clears the other
    // Inline math (Editor::InsertOfficeMath): the run's own text is literal
    // LaTeX-subset source (mep's existing math engine, LayoutMathExpression/
    // DrawMathLayout -- the same one org-mode/HTML <math> spans use), drawn
    // as a typeset expression instead of literal glyphs. Round-trips as
    // plain formatted text in DOCX/ODT (no OMML/MathML v1) -- opening the
    // saved file elsewhere shows the raw LaTeX source, a documented loss.
    bool math = false;
    OfficeFontFamily font_family = OfficeFontFamily::Sans;
    // 0 = inherit the paragraph's own default size (body text size, or a
    // heading's larger size) -- matches font_scale's own "0 means unset"
    // convention elsewhere in this codebase (html_doc.h's ComputedStyle).
    float font_size_pt = 0.0f;
    bool has_color = false;
    unsigned char color_r = 0, color_g = 0, color_b = 0;
    bool has_highlight = false;
    unsigned char highlight_r = 255, highlight_g = 255, highlight_b = 0;

    bool operator==(const DocFormat &o) const {
        return bold == o.bold && italic == o.italic && underline == o.underline && strike == o.strike &&
               superscript == o.superscript && subscript == o.subscript && math == o.math &&
               font_family == o.font_family && font_size_pt == o.font_size_pt && has_color == o.has_color &&
               color_r == o.color_r && color_g == o.color_g && color_b == o.color_b &&
               has_highlight == o.has_highlight && highlight_r == o.highlight_r && highlight_g == o.highlight_g &&
               highlight_b == o.highlight_b;
    }
    bool operator!=(const DocFormat &o) const { return !(*this == o); }
};

// Half-open [start,end) character range into DocParagraph::text. Spans in
// a paragraph's list are always kept non-overlapping and sorted by start
// -- every span-editing function below both assumes and preserves this.
struct DocSpan {
    int start = 0, end = 0;
    DocFormat fmt;
};

struct DocParagraph {
    // Flat UTF-8 text, like Buffer::lines[i]. An embedded '\t' represents
    // a preserved <w:tab/>/<text:tab/>; an embedded '\n' represents a
    // preserved soft line break (<w:br/>/<text:line-break/>) -- distinct
    // from the paragraph boundary itself, which is what splits paragraphs
    // in the first place. Neither is authorable via a v1 keybinding, only
    // preserved on round-trip if the source document already had one.
    std::string text;
    std::vector<DocSpan> spans;
    enum class Align { Left, Center, Right, Justify };
    Align align = Align::Left;
    int heading_level = 0;  // 0 = body text, 1-6 = heading
    // v1: one flat list level, bullet-or-numbered-or-neither -- no real
    // multi-level list-style definitions (matches this file's own
    // documented scope exclusion). Numbered's displayed number is always
    // "position within the current run of consecutive Numbered paragraphs"
    // (computed at render/save time, not stored), so inserting/deleting a
    // list item never leaves stale numbers behind.
    enum class ListKind { None, Bullet, Numbered };
    ListKind list_kind = ListKind::None;
    // Anchors a table/image (OfficeDoc::tables/images, below) to render
    // immediately after this paragraph -- -1 means none. A paragraph with
    // an anchor may still hold its own text (usually empty, e.g. right
    // after inserting a table at the cursor) so the anchor never has to be
    // its own zero-width paragraph type; paragraph-level cursor motion
    // treats an anchored table/image as a single step, never entering it
    // via the normal per-character column motions.
    int table_ref = -1;
    int image_ref = -1;
};

// A table's cells hold plain text only -- no per-cell rich formatting/
// nested tables/merged cells in v1 (this file's own documented scope
// exclusion). `cells` is always exactly rows*cols, row-major.
struct DocTable {
    int rows = 0, cols = 0;
    std::vector<std::string> cells;
    std::string &Cell(int r, int c) { return cells[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)]; }
    const std::string &Cell(int r, int c) const {
        return cells[static_cast<size_t>(r) * static_cast<size_t>(cols) + static_cast<size_t>(c)];
    }
};

// An embedded image: `bytes` is the original encoded (PNG/JPEG/...) file
// content, decoded on demand into a texture by the renderer (main.cpp),
// not eagerly here (this file is deliberately raylib-free, same reasoning
// as image_doc.h/pdf_doc.h). `natural_w`/`natural_h` are the decoded pixel
// dimensions, cached at insert/load time so layout doesn't need to decode
// just to know the aspect ratio. No resize handles/cropping in v1 -- always
// drawn at its natural size, capped to the pane's content width.
struct DocImage {
    std::string bytes;
    int natural_w = 0, natural_h = 0;
};

struct OfficeDoc {
    std::vector<DocParagraph> paragraphs;
    std::vector<DocTable> tables;
    std::vector<DocImage> images;
    std::string source_format;  // "docx" or "odt", drives which Save* to use
};

// ============================================================================
// Span-editing primitives -- pure functions over one DocParagraph, no I/O.
// Shared by Editor::HandleOffice{Normal,Insert,Visual}Input and by the
// parsers (which build spans directly rather than going through these).
// ============================================================================

// Deletes [a,b) from `p.text` and adjusts `p.spans` via the shared clamp
// rule: f(x) = x if x<=a; a if a<x<b; x-(b-a) if x>=b, applied to both
// span endpoints (s'=f(s), e'=f(e)); a span is dropped if s'==e' (unless
// it was already empty). This clamp is also reused by
// ToggleFormatOverRange to clip existing spans before inserting a new one
// for the toggled range -- it *is* the shared "cut a range out of a span
// list" primitive, not delete-specific.
void ApplyDeleteToParagraph(DocParagraph &p, int a, int b);

// Inserts `inserted` at `col`. A span ending at/before col is unchanged;
// a span starting exactly at col is NOT extended (new text doesn't
// retroactively join a span it's merely adjacent to at its start); a span
// strictly containing col (s<col<e) IS extended by len(inserted) -- "typing
// inside a bold word stays bold"; a span ending exactly at col is NOT
// extended either (no "sticky" insert format -- typing right after a bold
// word isn't bold; Visual-select-then-toggle is the only v1 way to apply
// formatting, deliberately, see the plan doc); spans starting at/after col
// shift by +len(inserted).
void ApplyInsertToParagraph(DocParagraph &p, int col, const std::string &inserted);

// Splits `p` at `col`, returning the new second paragraph; `p` is
// truncated in place to become the first half. A span straddling the cut
// (s<col<e) is itself split into two spans (one per resulting paragraph),
// both keeping the original format -- the common real-world case of
// pressing Enter mid-bold-run. Both halves inherit p's original
// align/heading_level/bullet (a deliberate v1 simplification vs. Word's
// actual "Enter after a Heading demotes the next paragraph" behavior).
DocParagraph SplitParagraphAt(DocParagraph &p, int col);

// Merges `next` onto the end of `p` in place; `next`'s spans shift by
// len(p.text) before appending (the result stays sorted because each
// paragraph's own span list was already sorted -- an invariant every
// function here assumes and preserves). `p` keeps its own paragraph
// properties (matches Word's own merge behavior).
void MergeParagraphs(DocParagraph &p, const DocParagraph &next);

// Toggles one boolean field of DocFormat (selected via pointer-to-member,
// so one function serves bold/italic/underline/strike alike) over [a,b):
// if the whole range is already uniformly on, turns it off; otherwise
// turns it on (matches Word's own convention for a mixed-format
// selection). Clips every overlapping span via ApplyDeleteToParagraph's
// same clamp rule, inserts a fresh span for [a,b) with the toggled
// format, then coalesces adjacent same-format spans -- not optional, since
// repeated toggling is the main way an editing session's span list would
// otherwise grow unboundedly.
void ToggleFormatOverRange(DocParagraph &p, int a, int b, bool DocFormat::*field);

// Merges adjacent/overlapping spans that have identical DocFormat into
// one. Exposed separately from ToggleFormatOverRange since parsers may
// also want to normalize a freshly-built span list.
void CoalesceSpans(std::vector<DocSpan> &spans);

// The DocFormat in effect at a single character offset (the span
// containing it, or a default DocFormat if none) -- used both by the
// renderer (per-run styling) and by ToggleFormatOverRange (determining
// on/off state).
DocFormat FormatAt(const DocParagraph &p, int col);

// Sets one non-boolean DocFormat field to an explicit value over [a,b) --
// the font-family/size/color/highlight counterpart of ToggleFormatOverRange
// (which only makes sense for a bool: "on"/"off" has no equivalent for
// "what size"). Unlike toggling, there's no "already uniform" state to
// detect first -- the caller (a toolbar dropdown/color-swatch click)
// already knows the exact value to apply, so this always sets it.
// `apply` receives a mutable DocFormat& to set the one field it closes
// over, mirroring ToggleFormatOverRange's pointer-to-member dispatch but
// generalized to a lambda since a non-bool field can't be toggled through
// a single shared code path the same way.
void SetFormatFieldOverRange(DocParagraph &p, int a, int b, const std::function<void(DocFormat &)> &apply);

// Sets every paragraph in [first,last] (inclusive paragraph indices) to
// `align`/`kind` -- the paragraph-level counterpart of the span-level
// setters above (alignment and list membership are whole-paragraph
// properties, not character-range ones, so there's no span math here at
// all, just a direct field assignment per paragraph).
void SetParagraphAlignment(std::vector<DocParagraph> &paragraphs, int first, int last, DocParagraph::Align align);
void SetParagraphListKind(std::vector<DocParagraph> &paragraphs, int first, int last, DocParagraph::ListKind kind);

// ============================================================================
// File I/O
// ============================================================================

bool IsDocxPath(const std::string &path);
bool IsOdtPath(const std::string &path);

// Sniffs `bytes` (a DocImage's raw encoded content) by magic-number prefix
// to get a lowercase extension ("png"/"jpeg"/"gif"/"bmp") -- ImageDoc::
// LoadFromMemory already validated the bytes decode as one of these at
// insert/load time, so this never needs to fail; unrecognized content
// falls back to "png" (a mismatched extension is cosmetic -- word/media/
// part names don't have to match their actual encoding -- not a
// correctness issue). Shared by SaveDocxToMemory's word/media/ part naming
// and SaveOdtToMemory's Pictures/ part naming, and their respective
// Content-Types/manifest MIME-type declarations.
std::string SniffImageExtension(const std::string &bytes);

// Maps a SniffImageExtension result ("png"/"jpeg"/"gif"/"bmp"/anything
// else) to its MIME type, for Content-Types/manifest declarations.
std::string MimeForImageExt(const std::string &ext);

// Reads a named entry out of an in-memory ZIP archive (both .docx and
// .odt are ZIP containers of XML parts). Returns false (out left empty)
// if the archive can't be opened or the entry isn't present. Shared
// between office_doc.cpp's DOCX parser and office_odt.cpp's ODT parser
// (and, eventually, Phase 4's save-back copy-through path).
bool ReadZipEntry(const unsigned char *zip_bytes, size_t zip_len, const char *entry_name,
                   std::vector<unsigned char> &out);

// Rebuilds a ZIP archive from `orig_bytes`, replacing exactly one entry
// (`entry_name`) with `new_content` and copying every other entry through
// unchanged via mz_zip_writer_add_from_zip_reader -- a raw central-
// directory-entry copy that preserves each original entry's compression
// method and (by iterating in the reader's own index order) position,
// rather than a generic extract-then-re-add loop that could silently
// re-compress something. This is what makes it safe for ODT too: ODF
// requires its `mimetype` entry be first and stored uncompressed, and
// since that entry is never the one being replaced, its raw copy
// preserves both properties by construction. Shared by
// office_doc.cpp's DOCX save-back and office_odt.cpp's ODT save-back.
bool WriteZipReplacingEntry(const unsigned char *orig_bytes, size_t orig_len, const char *entry_name,
                             const std::string &new_content, std::vector<unsigned char> &out, std::string &error);

// Same as WriteZipReplacingEntry but replaces every (name, content) pair in
// `entries` in one pass over the original archive, rather than requiring
// one full reader/writer round-trip per entry -- needed by XLSX save-back
// (sheet_xlsx.cpp), which may rewrite several xl/worksheets/sheetN.xml
// parts (one per sheet) in a single save. Any name in `entries` not found
// in the original archive is appended at the end (matching
// WriteZipReplacingEntry's own fallback), though v1's no-add/remove-sheets
// scope means that path is never actually exercised.
bool WriteZipReplacingEntries(const unsigned char *orig_bytes, size_t orig_len,
                               const std::vector<std::pair<std::string, std::string>> &entries,
                               std::vector<unsigned char> &out, std::string &error);

// Parses a .docx from memory into `out`. Tolerant of malformed/
// unsupported content the same way PdfDoc is -- individual bad
// paragraphs/runs are skipped rather than failing the whole load. Returns
// false (with `error` set) only if the file isn't a readable ZIP or has
// no `word/document.xml` part.
bool LoadDocxFromMemory(const unsigned char *bytes, size_t len, OfficeDoc &out, std::string &error);

// Serializes `doc.paragraphs`/`doc.tables` back into `word/document.xml`
// (re-parsing `original_bytes`'s copy of that part first, so the original
// root element's namespace declarations and a trailing <w:sectPr> -- page
// setup, which this model doesn't represent -- are preserved verbatim;
// only the <w:p>/<w:tbl> children are replaced) and rebuilds the full
// .docx ZIP via WriteZipReplacingEntry. A table round-trips (plain cell
// text only -- no per-cell formatting/merged cells, matching DocTable's own
// scope note); an image anchor is still dropped on save (DocImage isn't
// written back to word/media/ yet). A bullet paragraph's <w:numPr> also
// isn't re-emitted (no numbering.xml list-definition tracking in v1 --
// safer to drop the bullet's list formatting on save than guess an
// unresolvable numId that could trigger a "needs repair" prompt).
bool SaveDocxToMemory(const OfficeDoc &doc, const std::vector<unsigned char> &original_bytes,
                      std::vector<unsigned char> &out, std::string &error);

// Parses a .odt from memory into `out` (src/office_odt.cpp). Same
// tolerance convention as LoadDocxFromMemory -- individual bad
// paragraphs/styles are skipped rather than failing the whole load;
// returns false (with `error` set) only if the file isn't a readable ZIP
// or has no `content.xml` part.
bool LoadOdtFromMemory(const unsigned char *bytes, size_t len, OfficeDoc &out, std::string &error);

// Serializes `doc.paragraphs`/`doc.tables` back into `content.xml` (same
// preserve-everything-else-via-reparse approach as SaveDocxToMemory,
// scoped to <office:text>'s children) and rebuilds the full .odt ZIP via
// WriteZipReplacingEntry. Same known v1 losses as SaveDocxToMemory: an
// image anchor is dropped, and bullet paragraphs lose their <text:list>
// wrapping on save (no list-style tracking in v1) -- a table round-trips
// (plain cell text only, same scope as the DOCX path).
bool SaveOdtToMemory(const OfficeDoc &doc, const std::vector<unsigned char> &original_bytes,
                     std::vector<unsigned char> &out, std::string &error);

#endif
