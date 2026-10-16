#pragma once

// mep's own Zstandard (RFC 8878) decompressor -- decode only, no
// dictionaries. Written for Blender's compressed .blend files (Blender >=
// 3.0 writes zstd when "Compress" is on, as several frames followed by a
// skippable seek-table frame), but container-agnostic: give it any
// sequence of zstd and skippable frames.
//
// Supports every block type (raw, RLE, compressed), all literal section
// types (raw, RLE, Huffman 1- and 4-stream, treeless/repeat), all
// sequence table modes (predefined, RLE, FSE, repeat) and verifies the
// optional XXH64 content checksum. Frames that need a dictionary
// (Dictionary_ID != 0) are rejected.

#include <cstddef>
#include <cstdint>
#include <string>

namespace zstd {

// True if `data` starts with a zstd frame magic number (0xFD2FB528) or a
// skippable frame magic (0x184D2A5?).
bool LooksLikeZstd(const unsigned char *data, size_t len);

// Decompresses every frame in data[0..len) and appends the result to
// `out`. Returns false and sets *error (when given) on malformed input;
// `out` then holds whatever was decoded before the error.
// `max_output` bounds the total output size as a guard against
// decompression bombs (0 = no limit).
bool Decompress(const unsigned char *data, size_t len, std::string &out, std::string *error = nullptr,
                size_t max_output = 0);

// XXH64 of data[0..len) with the given seed (exposed for tests).
uint64_t Xxh64(const unsigned char *data, size_t len, uint64_t seed);

}  // namespace zstd
