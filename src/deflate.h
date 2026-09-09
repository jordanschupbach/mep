#pragma once

// mep's own in-house DEFLATE (RFC 1951) codec + CRC-32 + Adler-32 -- see
// MINIZ_REMOVAL_PLAN.md for the full writeup. A standalone, container-
// agnostic module (raw byte-in/byte-out): src/zip_archive.h wraps the
// raw-DEFLATE entry points in ZIP framing for office_doc.cpp/
// doc_export.cpp's DOCX/ODT/XLSX/ODS support, and the zlib-wrapped entry
// points are shared by STB_IMAGE_REMOVAL_PLAN.md's PNG decode/encode and
// gfx/backend_native_model_m3d.cpp's compressed mesh chunks (both use
// zlib framing, not ZIP's -- see that plan's Scoping decision 3).
//
// Deflate() only ever emits fixed Huffman blocks (see .cpp's own note on
// why: RFC 1951 permits it, real-world inflate implementations decode it
// identically to dynamic Huffman, and it sidesteps needing to build and
// transmit a dynamic Huffman table on the encode side for a real but
// modest compression-ratio cost that doesn't matter for mep's own small
// XML/text payloads -- see MINIZ_REMOVAL_PLAN.md's Non-goals).

#include <cstddef>
#include <cstdint>
#include <string>

namespace deflate {

uint32_t Crc32(uint32_t crc, const unsigned char *data, size_t len);
uint32_t Adler32(uint32_t adler, const unsigned char *data, size_t len);

// Raw DEFLATE (no zlib/gzip wrapper) -- the format ZIP entries use.
bool InflateRaw(const unsigned char *data, size_t len, std::string &out);
std::string DeflateRaw(const unsigned char *data, size_t len);

// zlib-wrapped DEFLATE (RFC 1950: 2-byte header, raw DEFLATE stream,
// 4-byte big-endian Adler-32 trailer) -- the format PNG's IDAT chunks
// and M3D's compressed mesh chunks use.
bool InflateZlib(const unsigned char *data, size_t len, std::string &out);
std::string DeflateZlib(const unsigned char *data, size_t len);

}  // namespace deflate
