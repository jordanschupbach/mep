#pragma once

// mep's own in-house PDF stream-filter decoder -- see
// PDFIUM_REMOVAL_PLAN.md. FlateDecode (+ PNG/TIFF Predictor) landed
// early, in Phase 3, since reading a PDF 1.5+ file's /Root at all
// depends on it (xdvipdfmx compresses nearly every small object into a
// compressed object stream). Phase 4 adds the rest of PDF's filter set
// (ASCIIHexDecode/ASCII85Decode/RunLengthDecode/LZWDecode/DCTDecode) and
// the general /Filter-array chain dispatch (DecodeStream).

#include "pdf_object.h"

#include <string>

namespace pdffilter {

// Inflates zlib-wrapped `raw` (via deflate::InflateZlib) and, if
// `decode_parms` is non-null and specifies `/Predictor` > 1, reverses a
// PNG (predictor 10-15, per-row filter-type byte, PNG spec section 6 --
// the predictor value itself doesn't distinguish which of the 5 PNG
// filter types was used per row, since that's carried in the row's own
// leading byte) or TIFF (predictor 2, horizontal differencing, no
// per-row tag byte) predictor using `/Columns` (default 1), `/Colors`
// (default 1), `/BitsPerComponent` (default 8) from decode_parms.
// decode_parms may be null (no predictor -- e.g. every xref stream in
// this plan's own Phase 1 fixtures uses none at all).
//
// Returns false only if the zlib stream itself is corrupt/truncated
// (deflate::InflateZlib's own failure); an unrecognized `/Predictor`
// value or inconsistent row-length math is tolerated by skipping
// predictor reversal and returning the plain inflated bytes, matching
// this codebase's "skip bad content, don't fail the whole document"
// convention.
bool FlateDecode(const std::string &raw, const pdfobj::Object *decode_parms, std::string *out);

// Decodes an ASCIIHexDecode stream: hex digit pairs (interior whitespace
// ignored, same tolerance as pdf_object.cpp's literal hex-string parser)
// up to an optional trailing `>` terminator (absence tolerated -- decode
// whatever's there). An odd trailing digit is padded with an implicit
// low nibble of 0, per spec 7.4.2.
bool ASCIIHexDecode(const std::string &raw, std::string *out);

// Decodes an ASCII85Decode stream (spec 7.4.3): groups of 5 base-85
// digits -> 4 bytes each, the `z` shorthand for an all-zero group, up to
// an optional `~>` terminator. A final partial group of 2-5 digits
// (padded internally with the highest-value digit before dividing,
// per spec) yields one fewer byte than digits.
bool ASCII85Decode(const std::string &raw, std::string *out);

// Decodes a RunLengthDecode stream (spec 7.4.5): a length byte 0-127
// means "copy the next length+1 literal bytes"; 129-255 means "repeat
// the following single byte 257-length times"; 128 is the EOD marker.
bool RunLengthDecode(const std::string &raw, std::string *out);

// Decodes an LZWDecode stream (spec 7.4.4): PDF's own LZW variant --
// MSB-first variable-width codes (9-12 bits, growing as the dictionary
// fills), a Clear-table code (256) and EOD code (257), `/EarlyChange`
// (default 1, from decode_parms) controlling whether the code width
// grows one code early. Distinct from gif_codec.cpp's own LZW (LSB-
// first bit order, different growth thresholds, no EarlyChange concept)
// despite both being "LZW" -- no code sharing between them beyond the
// general dictionary-scheme shape. Applies a PNG/TIFF `/Predictor` from
// decode_parms afterward, same as FlateDecode (LZWDecode can carry one
// too, per spec Table 8).
bool LZWDecode(const std::string &raw, const pdfobj::Object *decode_parms, std::string *out);

// Decodes a DCTDecode (baseline JPEG) stream via jpeg::Decode, returning
// RGBA8 pixels (alpha always 255) plus the image's own dimensions --
// kept as its own entry point rather than folded into DecodeStream's
// generic byte-in/byte-out chain, since per spec 7.4.8, DCTDecode is
// never combined with another filter in the same chain (it's always the
// sole or terminal filter for genuine image data), and its output needs
// interpreting as WxH pixels rather than treated as more encoded bytes.
bool DCTDecode(const std::string &raw, std::string *out_rgba, int *out_width, int *out_height);

// Applies every filter named in `stream_dict`'s `/Filter` (a single Name
// or an Array of Names) to `raw`, in order, using the matching
// `/DecodeParms` entry (a single Dict, an Array of Dicts/nulls parallel
// to `/Filter`, or absent) for each stage. Handles FlateDecode/
// ASCIIHexDecode/ASCII85Decode/RunLengthDecode/LZWDecode -- NOT
// DCTDecode (see DCTDecode's own doc comment for why that's a separate
// entry point) or the scanned-image formats this plan scopes out
// entirely (CCITTFaxDecode/JBIG2Decode/JPXDecode, see
// PDFIUM_REMOVAL_PLAN.md's Scoping decision 4).
//
// Returns false only if a stage that's supposed to produce more bytes
// fails outright (a corrupt Flate/LZW stream). An absent `/Filter`
// simply returns `raw` unchanged (true); an unrecognized filter name
// mid-chain is tolerated by passing that stage's input through
// unchanged, matching this codebase's "skip bad content" convention,
// EXCEPT that DCTDecode/CCITTFaxDecode/JBIG2Decode/JPXDecode named here
// short-circuit the whole call to false, since silently passing raw
// image-format bytes through as if they were plain data would corrupt
// whatever consumes the result -- callers that expect image data should
// route through DCTDecode (or accept the scoped-out non-goal) instead
// of calling DecodeStream on an image XObject's stream.
bool DecodeStream(const std::string &raw, const pdfobj::Object *stream_dict, std::string *out);

}  // namespace pdffilter
