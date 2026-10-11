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

// Whether a glyph name is nothing but a spelling of `code` itself, and
// so says nothing the code does not already say.
//
// Two conventions, both from producers that had no real name to give:
//   * `/a65`, `/g23`, `/cid120`, `/index7`, `/glyph42` -- a prefix and
//     the code in decimal.
//   * `/A0`, `/BT`, `/CG` -- the code in base 36, high digit as a
//     letter. (Verified against a real book: all 28 names sampled
//     decoded to exactly their own code.)
//
// The *code* is required, not just the name, so this can be proved
// rather than guessed: a name only loses to its code when it provably
// encodes that code. It decides whether a code whose name will not
// resolve may still be read as a character. dvips-style Type 3 bitmap
// fonts name every glyph this way, so the code is the only reading
// available and taking it recovers whole pages; a font with a real but
// unlisted name like `/sq` (a square) is saying something specific, and
// reading its code as a letter would invent text the page does not
// contain.
bool GlyphNameEncodesCode(const std::string &name, int code);

// An accented letter written as one character, given the base letter and
// the accent a PDF drew over it -- "e" plus an acute is U+00E9.
//
// PDFs routinely draw an accent as its own glyph positioned over the
// letter, because that is how the font is built: one Type 1 program
// with `e` and `acute` in it and no `eacute` at all. Extracted glyph by
// glyph that reads as "FRE\u00b4DE\u00b4RIC", which is not a word any
// search will find and not what a reader copying the name wants.
//
// `accent` is the *spacing* form, which is what the glyph's own
// /ToUnicode or glyph name gives (U+00B4 ACUTE ACCENT, not U+0301
// COMBINING ACUTE). Returns 0 when the pair has no single-character
// form, which is the caller's signal to leave both alone.
int ComposeAccented(int base, int accent);

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
