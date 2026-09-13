#pragma once

// mep's own in-house PDF cross-reference table -- see
// PDFIUM_REMOVAL_PLAN.md Phase 3. Turns a PDF's "startxref" trailer
// pointer into a full object-number -> byte-offset (or -> containing
// ObjStm) map, by parsing classic xref tables, cross-reference streams
// (PDF 1.5+), or -- if neither is parseable -- a brute-force scan for
// "N G obj" patterns across the whole file. Xref-stream decoding depends
// on pdf_filters.h's FlateDecode (confirmed necessary even for the most
// basic /Root lookup: Phase 1/3's own tectonic fixtures compress nearly
// every small object, /Root and /Info included, into one compressed
// object stream (ObjStm) -- there is no way to read a modern PDF 1.5+
// file's catalog without decompressing at least one stream first).

#include "pdf_crypt.h"
#include "pdf_object.h"

#include <cstddef>
#include <map>
#include <memory>
#include <string>

namespace pdfxref {

enum class EntryKind { Free, InUse, Compressed };

struct Entry {
    EntryKind kind = EntryKind::Free;
    long long offset = 0;           // InUse: byte offset of "N G obj" in the file
    int gen = 0;                    // InUse: generation number
    long long stream_num = 0;       // Compressed: object number of the containing ObjStm
    long long index_in_stream = 0;  // Compressed: this object's index within that ObjStm
};

// The document-wide cross-reference table + merged trailer dict.
class XrefTable {
public:
    // Parses `data`/`len` starting from its "startxref" trailer offset,
    // walking the /Prev chain (classic and stream sections may be mixed
    // across an incrementally-updated file) with cycle protection so a
    // malformed circular /Prev chain can't loop forever. Falls back to
    // RecoverByScanning if "startxref"/the section it points to can't be
    // parsed at all. Always leaves *some* state (possibly empty) rather
    // than signaling failure -- matches this codebase's tolerant
    // convention; callers that care check Entries().empty() or whether
    // Trailer().Find("Root") resolves to anything.
    void Load(const unsigned char *data, size_t len);

    // Brute-force recovery: scans the whole buffer for "N G obj"
    // patterns (a later file-position occurrence of the same object
    // number wins, matching incremental-update "last revision is
    // authoritative" semantics), and reconstructs a trailer -- from a
    // `trailer` keyword's dict if one is found anywhere in the buffer,
    // else by locating a recovered object whose value has
    // `/Type /Catalog` and pointing /Root at it directly. Public (not
    // just an internal Load() fallback) so a caller can force recovery
    // even when a xref section technically parses but looks wrong.
    void RecoverByScanning(const unsigned char *data, size_t len);

    const Entry *Find(int num) const;
    const pdfobj::Object &Trailer() const { return trailer_; }
    const std::map<int, Entry> &Entries() const { return entries_; }

    // Whether the trailer has an /Encrypt entry at all (regardless of
    // whether SetupEncryption below actually managed to authenticate an
    // empty password against it) -- PDFIUM_REMOVAL_PLAN.md Phase 12.
    // Distinguishes "not encrypted" from "encrypted, but Encryption()
    // below is null because it needs a real password / uses an
    // unsupported scheme" for a caller (pdfdoc::PdfDocument::Load, and
    // eventually Phase 13's pdf_doc.cpp swap-in) that needs to tell the
    // two apart -- e.g. to report a password-required error rather than
    // just "this document has 0 pages" with no explanation.
    bool IsEncrypted() const { return is_encrypted_; }

    // Non-null only once IsEncrypted() is true AND an empty user
    // password successfully authenticated against it (pdf_crypt.h's
    // SetupStandardSecurityHandler) -- ResolveObject/ResolveStream below
    // decrypt every string/stream through this automatically; a caller
    // never needs to touch it directly except to detect "IsEncrypted()
    // but this is null" (wrong/real password, or an unsupported
    // encryption scheme -- both reported identically, matching PDFium's
    // own FPDF_ERR_PASSWORD outcome for both cases).
    const pdfcrypt::EncryptionState *Encryption() const { return encryption_.get(); }

private:
    std::map<int, Entry> entries_;
    pdfobj::Object trailer_;
    bool is_encrypted_ = false;
    std::unique_ptr<pdfcrypt::EncryptionState> encryption_;

    // Reads /Encrypt (if any) off trailer_ and, if present, attempts to
    // authenticate an empty user password against it -- called at the
    // tail of both Load() and RecoverByScanning() (the latter both as
    // Load()'s own fallback and as a standalone entry point in its own
    // right, per its own public doc comment) so encryption_ is always
    // populated from whatever trailer_ each of them actually ends up
    // with; deliberately not guarded against being called twice for the
    // same document (Load() falling back to RecoverByScanning still
    // calls this a second time at Load()'s own tail) -- a few dozen
    // bytes' worth of MD5, re-run once more, is not worth threading
    // extra state around to avoid.
    void SetupEncryption(const unsigned char *data, size_t len);

    // Parses one xref section (classic table or stream) at `offset`,
    // merging its entries into entries_ (existing object numbers are
    // NOT overwritten -- the first section merged in, which Load()
    // always starts from the most recent, wins per spec) and its
    // trailer keys into trailer_ (same first-wins rule). Returns the
    // section's /Prev offset via *out_prev (and *out_has_prev), and
    // whether the section itself was parseable at all.
    bool LoadSection(const unsigned char *data, size_t len, long long offset, long long *out_prev,
                      bool *out_has_prev);
    bool LoadClassicTable(const unsigned char *data, size_t len, size_t pos, long long *out_prev,
                           bool *out_has_prev);
    bool LoadXrefStream(const unsigned char *data, size_t len, size_t pos, long long *out_prev,
                         bool *out_has_prev);
    void MergeTrailer(const pdfobj::Object &section_trailer);
};

// Resolves object (num, gen) to its parsed value using `table`,
// following through an ObjStm for Compressed entries. Returns a Null
// Object (matching pdfobj's own tolerant-failure convention) if the
// entry is Free/absent or fails to parse in any way.
//
// `gen` is accepted for interface symmetry with PDF's own "N G R"
// addressing but not checked against the entry's recorded generation --
// multi-generation object reuse only matters for incrementally-updated
// files with a freed-and-reallocated object slot, which is out of this
// plan's scope (every real target-producer fixture uses generation 0
// throughout, confirmed in Phase 1).
pdfobj::Object ResolveObject(const unsigned char *data, size_t len, const XrefTable &table, int num, int gen = 0);

// Like ResolveObject, but for a stream object (one with an actual
// `stream`/`endstream` body -- a page's /Contents, a Form/Image
// XObject, an embedded font file, ...) also returns its raw,
// NOT-YET-filter-decoded bytes (see pdf_filters.h for that step).
// `*out_dict` is always populated with the resolved value if it's a
// dict, even when this returns false (matching ResolveObject's own
// tolerant style, so a caller that just wants to check /Type or /Subtype
// first doesn't need two calls) -- it only returns true when a raw
// stream body actually exists. A Compressed (in-ObjStm) entry can never
// be a stream per spec (streams are always direct top-level indirect
// objects), so this always returns false for one.
bool ResolveStream(const unsigned char *data, size_t len, const XrefTable &table, int num, int gen,
                    pdfobj::Object *out_dict, std::string *out_raw_stream);

}  // namespace pdfxref
