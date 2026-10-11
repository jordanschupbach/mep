#pragma once

// Is a page's extracted text actually text?
//
// Some PDFs hand back characters that are not what the page says. The
// Art of Computer Programming vol. 4A is the case this was written for:
// every font in it is a Type 3 bitmap with no /ToUnicode, and the codes
// resolve through the font's own encoding to arbitrary letters, so a
// page of combinatorics extracts as
//
//     C C C C
//     D D D B D D D D
//     D D D D D D D
//
// Nothing is missing in the sense the earlier work measured -- the
// glyphs are not being dropped, they are being mapped to lies. So an
// unmapped-glyph counter cannot see this, and neither can comparing
// against poppler, which produces its own different garbage ("$? @ @ @
// @&") from the same file.
//
// What separates it is diversity. Measured with `mep-pdf-probe
// quality` over about 2,500 pages of real books -- textbooks, theses,
// a scanned CCITT facsimile, two novels -- and the generated fixtures,
// not one page of genuine text scored below 33 distinct non-space
// characters, and not one was flagged. Those TAOCP pages score three to
// five, and 457 of its 901 pages trip. The gap is wide enough to act
// on.
//
// Used to decide whether asking a model about the page should quote its
// text or send the page as a picture instead. Erring toward "it is
// fine" is the safe direction: a false positive sends an image for a
// page whose text was usable, which costs tokens and loses the exact
// wording.

#include <cstddef>
#include <string>

namespace textq {

struct Quality {
    // Non-whitespace characters considered.
    size_t chars = 0;
    // How many distinct byte values appear among them. Bytes rather
    // than codepoints is exact for the only case that can be judged,
    // since `nonascii` below must be zero for that.
    size_t distinct = 0;
    // Decimal digits, so a legitimate page of numbers -- a table of
    // results, a log -- is not judged by the same yardstick as prose.
    size_t digits = 0;
    // Bytes outside ASCII, i.e. any non-Latin script or typographic
    // character. One is enough to exempt the text from judgement
    // entirely -- see Degenerate().
    size_t nonascii = 0;

    // Whether this looks like mis-mapped glyphs rather than text.
    //
    // Three conditions, all required:
    //   * enough text to judge at all. A short run genuinely can use
    //     few distinct characters ("SEE ALSO"), and a selection of two
    //     words must never be called garbage.
    //   * implausibly few distinct characters. The threshold sits far
    //     below every real page measured (33) and far above the garbage
    //     (4).
    //   * not mostly digits, which is what saves a numeric table: ten
    //     digits, a separator and a sign is a dozen distinct characters
    //     by itself, and that page's text is perfectly good.
    //   * nothing outside ASCII. Mis-mapped glyphs land on ASCII
    //     letters, because what resolves them is a glyph-name table
    //     whose entries are Latin; a page carrying Greek, Cyrillic, CJK
    //     or even a typographic dash is therefore saying something.
    //     This is a real exemption rather than a hedge -- without it,
    //     five Greek letters repeated score six distinct bytes and the
    //     page would be called garbage, which an earlier version of
    //     this did.
    bool Degenerate() const {
        return chars >= kMinChars && distinct <= kMaxDistinct && digits * 4 < chars && nonascii == 0;
    }

    static constexpr size_t kMinChars = 200;
    static constexpr size_t kMaxDistinct = 12;
};

inline Quality Measure(const std::string &text) {
    Quality q;
    bool seen[256] = {};
    for (char raw : text) {
        const unsigned char c = static_cast<unsigned char>(raw);
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v') continue;
        ++q.chars;
        if (!seen[c]) {
            seen[c] = true;
            ++q.distinct;
        }
        if (c >= '0' && c <= '9') ++q.digits;
        if (c >= 0x80) ++q.nonascii;
    }
    return q;
}

}  // namespace textq
