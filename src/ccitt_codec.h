#pragma once

// mep's own in-house CCITT Group 3/4 fax decoder -- the codec behind
// PDF's own CCITTFaxDecode filter (spec 7.4.6), which
// PDFIUM_REMOVAL_PLAN.md's Scoping decision 4 originally left out
// along with JBIG2/JPX. It came back in because it is not a niche
// scanned-document format in practice: a typeset book run through
// Acrobat stores its line art this way, and leaving it out drops every
// one of those figures silently (the Causality.pdf fixture this was
// written against has 95 of its 134 image XObjects in CCITT, over
// 89 pages whose figures and tables simply did not draw).
//
// Implements ITU-T T.4 (Group 3, both the one-dimensional Modified
// Huffman coding and the two-dimensional Modified READ extension) and
// T.6 (Group 4, pure two-dimensional) -- i.e. every value PDF's own
// `/K` parameter selects between. Deliberately dependency-free, same
// as deflate.h/jpeg_codec.h/png_codec.h, and takes/returns plain
// std::string buffers so pdf_filters.cpp can drop it straight into
// DecodeStream's own filter chain.
//
// Does NOT implement: uncompressed mode (T.4 Annex / the `0000001111`
// extension code, which no real producer emits), or error-correcting
// resynchronisation beyond "stop at the first unreadable row". A
// truncated or damaged stream decodes as far as it can and leaves the
// remaining rows white, matching this codebase's "render what you can,
// skip what you can't" convention rather than failing a whole page
// over one bad figure.

#include <cstddef>
#include <string>

namespace ccitt {

// The subset of PDF's own CCITTFaxDecode /DecodeParms this decoder
// needs (spec 7.4.6, Table 11). Defaults match the spec's own.
struct Params {
    // < 0: pure two-dimensional (Group 4, T.6).
    //   0: pure one-dimensional (Group 3, T.4 Modified Huffman).
    // > 0: mixed -- each line carries a tag bit after its EOL saying
    //      whether it is coded one- or two-dimensionally.
    int k = 0;
    int columns = 1728;
    // Expected row count. 0 means "not stated": decode until the data
    // runs out. A stated value also bounds a damaged stream.
    int rows = 0;
    // false (the spec's default): 0 bits are black in the decoded data.
    bool black_is_1 = false;
    // Each row's encoding begins on a byte boundary.
    bool byte_align = false;
};

// Decodes `raw` into packed 1-bit-per-pixel samples, MSB first, with
// every row starting on a byte boundary ((columns + 7) / 8 bytes per
// row) -- exactly the sample layout a PDF image with
// /BitsPerComponent 1 expects, so the result drops straight into
// pdf_content.cpp's own UnpackSamples.
//
// `*out_rows` receives the number of rows actually decoded. When
// `p.rows` was stated, `*out` is always that many rows tall (any row
// the data didn't reach is left white), so the caller's own
// /Width x /Height expectation always holds.
//
// Returns false only when nothing at all could be decoded (no columns,
// or not one readable row); a stream that dies partway through still
// returns true with the rows it managed.
bool Decode(const std::string &raw, const Params &p, std::string *out, int *out_rows);

}  // namespace ccitt
