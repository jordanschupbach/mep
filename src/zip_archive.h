#pragma once

// mep's own in-house ZIP container reader/writer -- see
// MINIZ_REMOVAL_PLAN.md for the full writeup. Used by office_doc.cpp's
// ReadZipEntry/WriteZipReplacingEntry(ies) (shared in turn by
// office_odt.cpp, sheet_xlsx.cpp, sheet_ods.cpp -- every DOCX/ODT/XLSX/
// ODS format is a ZIP of XML parts) and by doc_export.cpp's ODT export
// path. DEFLATE itself lives in deflate.h -- this module is purely the
// ZIP container framing (local file headers, central directory, end-of-
// central-directory record) on top of it.
//
// BuildZipArchive was originally doc_export.cpp's own hand-written ZIP
// writer (moved here unchanged in behavior, just relocated so
// office_doc.cpp's writer can reuse it too) -- see its own comment for
// why it always writes real sizes/CRC in the local header instead of a
// trailing data descriptor: a real LibreOffice round-trip rejected the
// data-descriptor form.

#include <cstddef>
#include <string>
#include <vector>

namespace zip {

struct EntryToWrite {
    std::string name;
    std::string data;
    bool store = false;  // force STORED (no compression) rather than DEFLATE
};

// Builds a complete ZIP archive from `entries`, deflating each unless
// marked `store` or DEFLATE fails to shrink it (falls back to STORED).
std::string BuildArchive(const std::vector<EntryToWrite> &entries);

// Extracts one entry by exact name match. Returns false if the archive
// is malformed or the entry isn't found.
bool Extract(const unsigned char *zip_bytes, size_t zip_len, const char *entry_name, std::string &out);

// Lists every entry's name + decompressed content, in central-directory
// order -- the "read everything, splice in a replacement, rebuild via
// BuildArchive" pattern office_doc.cpp's WriteZipReplacingEntry(ies) use.
bool ListAll(const unsigned char *zip_bytes, size_t zip_len, std::vector<EntryToWrite> &out, std::string &error);

}  // namespace zip
