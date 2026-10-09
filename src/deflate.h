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
// `out_consumed`, when given, receives how many input bytes the stream
// actually occupied (rounded up to a whole byte, since a DEFLATE stream
// need not end on a byte boundary) -- what a caller needs to find
// whatever follows it, e.g. InflateZlib's own Adler-32 trailer when the
// buffer it was handed is longer than the stream itself.
bool InflateRaw(const unsigned char *data, size_t len, std::string &out, size_t *out_consumed = nullptr);
std::string DeflateRaw(const unsigned char *data, size_t len);

// zlib-wrapped DEFLATE (RFC 1950: 2-byte header, raw DEFLATE stream,
// 4-byte big-endian Adler-32 trailer) -- the format PNG's IDAT chunks
// and M3D's compressed mesh chunks use.
//
// `len` may be longer than the stream: the Adler-32 is read from
// immediately after the DEFLATE data rather than from the end of the
// buffer. That matters for PDF, where a stream object's own /Length
// routinely counts the EOL separating the data from `endstream` -- one
// stray byte used to shift the trailer out from under this check and
// fail an image that had decoded perfectly (every /FlateDecode figure
// in the Causality.pdf fixture's last chapter did exactly that). A
// trailer that is missing entirely (a truncated stream) is accepted
// with the bytes recovered so far; one that is present but wrong is
// still a failure.
bool InflateZlib(const unsigned char *data, size_t len, std::string &out);
std::string DeflateZlib(const unsigned char *data, size_t len);

}  // namespace deflate
