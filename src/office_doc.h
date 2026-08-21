#ifndef MEP_OFFICE_DOC_H
#define MEP_OFFICE_DOC_H

#include <cstddef>
#include <string>
#include <utility>
#include <vector>

// Deliberately raylib-free (same reasoning as image_doc.h/pdf_doc.h): the
// document model, parsing, and span-editing logic are pure CPU-side data
// structures, usable/testable without a GL context. main.cpp is the only
// place that turns a paragraph's spans into rendered/word-wrapped text.
//
// A hand-rolled, intentionally partial rich-text model for .docx/.odt --
// not a general-purpose office-document library. No tables, images,
// headers/footers, footnotes/comments, track changes, real numbered
// lists (bullet-or-not only), font-family/size/color choice beyond a few
// heading sizes, or full OOXML/ODF style-cascade inheritance. Legacy
// binary .doc is out of scope entirely. See NVIM_PARITY_PLAN.md's Office
// pane phase for the full exclusion list and rationale.

struct DocFormat {
    bool bold = false, italic = false, underline = false, strike = false;
    bool operator==(const DocFormat &o) const {
        return bold == o.bold && italic == o.italic && underline == o.underline && strike == o.strike;
    }
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
    bool bullet = false;    // v1: bullet-or-not only, no real numbering
};

struct OfficeDoc {
    std::vector<DocParagraph> paragraphs;
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

// ============================================================================
// File I/O
// ============================================================================

bool IsDocxPath(const std::string &path);
bool IsOdtPath(const std::string &path);

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

// Serializes `doc.paragraphs` back into `word/document.xml` (re-parsing
// `original_bytes`'s copy of that part first, so the original root
// element's namespace declarations and a trailing <w:sectPr> -- page
// setup, which this model doesn't represent -- are preserved verbatim;
// only the <w:p> paragraph children are replaced) and rebuilds the full
// .docx ZIP via WriteZipReplacingEntry. Known v1 loss: a <w:tbl>/image
// present in `original_bytes` is dropped on save (never represented in
// OfficeDoc in the first place -- see the V1 scope exclusions), and a
// bullet paragraph's <w:numPr> isn't re-emitted (no numbering.xml
// list-definition tracking in v1 -- safer to drop the bullet's list
// formatting on save than guess an unresolvable numId that could trigger
// a "needs repair" prompt).
bool SaveDocxToMemory(const OfficeDoc &doc, const std::vector<unsigned char> &original_bytes,
                      std::vector<unsigned char> &out, std::string &error);

// Parses a .odt from memory into `out` (src/office_odt.cpp). Same
// tolerance convention as LoadDocxFromMemory -- individual bad
// paragraphs/styles are skipped rather than failing the whole load;
// returns false (with `error` set) only if the file isn't a readable ZIP
// or has no `content.xml` part.
bool LoadOdtFromMemory(const unsigned char *bytes, size_t len, OfficeDoc &out, std::string &error);

// Serializes `doc.paragraphs` back into `content.xml` (same
// preserve-everything-else-via-reparse approach as SaveDocxToMemory,
// scoped to <office:text>'s children) and rebuilds the full .odt ZIP via
// WriteZipReplacingEntry. Same known v1 losses as SaveDocxToMemory: a
// table/image in the original is dropped, and bullet paragraphs lose
// their <text:list> wrapping on save (no list-style tracking in v1).
bool SaveOdtToMemory(const OfficeDoc &doc, const std::vector<unsigned char> &original_bytes,
                     std::vector<unsigned char> &out, std::string &error);

#endif
