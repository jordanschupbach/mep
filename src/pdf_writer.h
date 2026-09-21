#pragma once

// mep's own in-house PDF *writer* -- the counterpart to pdf_object.h's
// parser. Two layers:
//
//   1. A value/object serializer (the inverse of pdfobj::ParseObject):
//      turns a pdfobj::Object back into conforming PDF syntax.
//   2. An incremental-update builder (PDF spec 7.5.6): given a loaded
//      document and a set of markup annotations to add, produces a NEW
//      complete file whose original bytes are untouched and which has a
//      fresh revision appended (new annotation objects + rewritten page
//      dicts + a classic cross-reference section whose /Prev chains back
//      to the file's existing xref). This is what lets the viewer save
//      user highlights/notes back into the PDF's /Annots without a
//      round-trip through a full rewrite.
//
// Scope note: the appended section is a CLASSIC `xref` table + trailer
// even when the base file uses cross-reference streams. mep's own reader
// merges mixed classic/stream /Prev chains (pdf_xref.cpp), and a classic
// trailer whose /Prev points at an xref stream is accepted by Adobe/
// Chrome/poppler -- far simpler than emitting an xref stream. Encrypted
// files are refused (pdfcrypt is decrypt-only for AES, and page dicts are
// decrypted on read so can't be re-emitted verbatim into a cipher file).

#include "pdf_annots.h"
#include "pdf_document.h"
#include "pdf_object.h"
#include "pdf_xref.h"

#include <cstddef>
#include <ctime>
#include <string>

namespace pdfwrite {

// -- Serializer -------------------------------------------------------

// Appends the textual encoding of a single value (no "N G obj"/"endobj"
// wrapper). The inverse of pdfobj::ParseObject: Null->"null", numbers,
// names (with #xx escaping), strings (literal, escaped), arrays, dicts,
// and "N G R" references all round-trip.
void SerializeValue(const pdfobj::Object &v, std::string *out);
std::string SerializeValue(const pdfobj::Object &v);

// Encodes one indirect object body: "num gen obj\n<value>\nendobj\n". If
// `stream_body` is non-null, emits a stream instead: `dict` is augmented
// with /Length (== the emitted body size) and, when `compress`, with
// /Filter /FlateDecode (the body is deflate::DeflateZlib-compressed).
std::string SerializeIndirectObject(int num, int gen, const pdfobj::Object &dict,
                                    const std::string *stream_body = nullptr, bool compress = false);

// A PDF real, never in exponent notation (1e-5 is illegal PDF syntax):
// fixed 5 fractional digits, trailing zeros and a trailing '.' trimmed,
// and -0 normalised to 0.
std::string FormatReal(double x);

// Escapes a name's body (no leading '/'): every byte outside the "regular
// character" set (delimiters, whitespace, '#', or non-0x21..0x7E) becomes
// #xx.
std::string EncodeName(const std::string &name);

// A PDF literal string body including the surrounding parentheses:
// backslash-escapes '(', ')', '\\', emits \r \n \t, and \ddd octal for
// other non-printables. Accepts arbitrary bytes.
std::string EncodeLiteralString(const std::string &bytes);

// A PDF date string, "D:YYYYMMDDHHmmSS+00'00'" (UTC), spec 7.9.4.
std::string PdfDateString(std::time_t t);

// -- Incremental update ----------------------------------------------

// One annotation to remove from its page's /Annots array (by object
// number). The referenced object simply becomes unreferenced (spec-legal);
// deleting a highlight also removes its note, since a highlight carries its
// note in its own /Contents.
struct AnnotDelete {
    int page = 0;      // 0-based page index the annotation is on
    int obj_num = 0;   // the annotation's object number (pdfannots::PdfAnnot::src_obj)
};

// Returns the full new file (original `data`/`len` verbatim + one appended
// incremental-update revision), or an empty string with *err set on
// refusal/failure (currently: encrypted documents). Three kinds of change:
//   - `adds`   : new annotations (from_file==false) -> fresh objects appended, refs added to their page /Annots.
//   - `edits`  : existing file annotations (from_file==true, src_obj>0) -> the rebuilt dict is re-emitted at the
//                SAME object number (its old /Annots ref still points there, now shadowed); no new ref added.
//   - `deletes`: object numbers removed from their page's /Annots array.
// Any page touched by an add/edit/delete has its dict rewritten. `table`/
// `document` must have been loaded from `data`/`len`. `now` timestamps
// annotations whose `modified` is empty and the new trailer /ID (pass a
// fixed value in tests for determinism).
std::string BuildIncrementalUpdate(const unsigned char *data, size_t len, const pdfxref::XrefTable &table,
                                   const pdfdoc::PdfDocument &document, const std::vector<pdfannots::PdfAnnot> &adds,
                                   const std::vector<pdfannots::PdfAnnot> &edits,
                                   const std::vector<AnnotDelete> &deletes, std::time_t now, std::string *err);

}  // namespace pdfwrite
