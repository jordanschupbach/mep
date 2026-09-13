#pragma once

// mep's own in-house PDF simple-font encoding tables -- see
// PDFIUM_REMOVAL_PLAN.md Phase 10. Three of PDF's predefined encodings
// (spec Annex D): StandardEncoding (the old Adobe/PostScript default),
// WinAnsiEncoding (~= Windows code page 1252), MacRomanEncoding (~=
// classic Mac OS Roman) -- each a code (0-255) -> glyph name table, used
// as a simple font's base encoding before /Differences overrides are
// applied. Also a small Adobe-Glyph-List-style name -> Unicode resolver
// (covering exactly the names these 3 tables produce, plus the `uniXXXX`
// numeric-escape convention real /Differences arrays sometimes use for
// anything else) and the CFF spec's 391 predefined standard strings
// (Appendix A) -- needed to turn a CFF font's own charset SIDs back into
// glyph names for name-based code->GID resolution on a simple CFF font.

#include <string>

namespace pdfenc {

enum class Base { kStandard, kWinAnsi, kMacRoman };

// Glyph name for `code` (0-255) under encoding `base`, or nullptr if
// that code is undefined in that encoding (a real, sparse condition for
// StandardEncoding especially -- not an error).
const char *EncodingName(Base base, int code);

// Adobe-Glyph-List-style resolution: returns the Unicode codepoint for
// `name`, or -1 if unrecognized. Handles the ~160 names that
// EncodingName can produce directly, plus the `uniXXXX` (exactly 4 hex
// digits) and `uXXXX`/`uXXXXX`/`uXXXXXX` (4-6 hex digits) numeric-escape
// conventions AGL-aware tools (and real-world /Differences arrays) use
// for anything outside that set.
int GlyphNameToUnicode(const std::string &name);

// CFF predefined standard string for SID 0 up to (but not including)
// CffStandardStringCount() (CFF spec Appendix A, ~391 entries -- this
// project's own transcription, self-sized rather than hardcoded to a
// magic count so an off-by-one in transcribing ~391 rarely-used entries
// can't silently misalign the table), or nullptr for an out-of-range
// SID. A real embedded CFF font's charset commonly reuses these for
// ordinary Latin glyph names (SIDs 1-228, exactly the ISOAdobe charset
// order -- independently verifiable against EncodingName(kStandard, ...)
// glyph-for-glyph, see pdf_encodings_test.cpp) even in an otherwise-
// custom charset, since it's cheaper than a custom String INDEX entry
// for anything already in this table. SIDs at/above
// CffStandardStringCount() index into that font's own String INDEX
// instead (a `gfx::cff::FontInfo` concern, not this table's).
const char *CffStandardString(int sid);

// Number of predefined standard strings this table actually holds.
int CffStandardStringCount();

}  // namespace pdfenc
