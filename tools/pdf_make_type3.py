#!/usr/bin/env python3
"""Author a PDF whose text is set in a Type 3 font, by hand.

Called by tools/pdf_make_fixtures.sh. Separate from it because no modern
TeX engine emits Type 3 *text* fonts any more, so this fixture cannot be
generated the way the others are -- and because a Type 3 font is small
enough to write out directly.

It matters because a Type 3 glyph is a content-stream procedure rather
than an outline, and in real files (old dvips/pk output, some figure
generators) that procedure draws an ImageMask bitmap. 79 Type 3
instances turned up in a 120-file sample of real library PDFs. They are
the classic "the maths on this page is a low-resolution bitmap" case: a
bitmap glyph has one fixed resolution, so it cannot sharpen as the page
is zoomed the way an outline does.

The page deliberately shows the same four glyphs at 10pt, 24pt and 48pt,
so a render comparison can see how the bitmap holds up as it is scaled,
and includes one line of ordinary Type 1 text so the page exercises both
font kinds at once.
"""

import sys

# One 8x8 ImageMask per glyph: bit set == painted, rows top to bottom,
# each row padded to a byte boundary (spec 8.9.3). Deliberately crisp
# geometric shapes -- a diagonal and a dither are the patterns that show
# resampling damage most clearly.
GLYPHS = {
    "square": [0xFF, 0x81, 0x81, 0x81, 0x81, 0x81, 0x81, 0xFF],
    "tri": [0x18, 0x18, 0x3C, 0x3C, 0x7E, 0x7E, 0xFF, 0xFF],
    "diag": [0x80, 0x40, 0x20, 0x10, 0x08, 0x04, 0x02, 0x01],
    "dots": [0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55, 0xAA, 0x55],
}
ORDER = ["square", "tri", "diag", "dots"]


def glyph_proc(bits):
    """One glyph's procedure: declare the advance with d0, then paint an
    inline ImageMask over the whole 8x8 cell. ASCIIHexDecode keeps the
    stream printable, which makes the fixture readable if it ever needs
    debugging by eye."""
    body = bytes(bits).hex().encode()
    return (
        b"800 0 d0\n"
        b"q 800 0 0 800 0 0 cm\n"
        b"BI /W 8 /H 8 /IM true /BPC 1 /D [0 1] /F /AHx ID\n" + body + b">\nEI Q\n"
    )


def build():
    objs = {}

    # Glyph procedures first, so their object numbers are known when the
    # /CharProcs dict that references them is built.
    charprocs_refs = {}
    num = 10
    for g in ORDER:
        proc = glyph_proc(GLYPHS[g])
        objs[num] = (b"<< /Length %d >>" % len(proc), proc)
        charprocs_refs[g] = num
        num += 1

    content = b"\n".join(
        [
            b"BT /T3 24 Tf 72 700 Td (abcd) Tj ET",
            b"BT /T3 48 Tf 72 620 Td (abcd) Tj ET",
            b"BT /T3 10 Tf 72 560 Td (abcdabcdabcd) Tj ET",
            b"BT /F1 12 Tf 72 520 Td (Ordinary Type 1 text on the same page as Type 3 glyphs.) Tj ET",
        ]
    ) + b"\n"

    charprocs = b"<< " + b" ".join(b"/%s %d 0 R" % (g.encode(), charprocs_refs[g]) for g in ORDER) + b" >>"
    # 'a'..'d' map onto the four glyph names, so the content stream can
    # show them as ordinary text.
    encoding = b"<< /Type /Encoding /Differences [97 " + b" ".join(b"/" + g.encode() for g in ORDER) + b"] >>"

    objs[1] = (b"<< /Type /Catalog /Pages 2 0 R >>", None)
    objs[2] = (b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>", None)
    objs[3] = (
        b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
        b" /Resources << /Font << /T3 5 0 R /F1 9 0 R >> >>"
        b" /Contents 4 0 R >>",
        None,
    )
    objs[4] = (b"<< /Length %d >>" % len(content), content)
    # FontMatrix scales the 800-unit glyph space to the 1000-unit text
    # space a Tf size is expressed in.
    objs[5] = (
        b"<< /Type /Font /Subtype /Type3 /Name /T3"
        b" /FontBBox [0 0 800 800] /FontMatrix [0.001 0 0 0.001 0 0]"
        b" /CharProcs 6 0 R /Encoding 7 0 R"
        b" /FirstChar 97 /LastChar 100 /Widths 8 0 R >>",
        None,
    )
    objs[6] = (charprocs, None)
    objs[7] = (encoding, None)
    objs[8] = (b"[800 800 800 800]", None)
    objs[9] = (b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>", None)

    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    offsets = {}
    for n in sorted(objs):
        body, stream = objs[n]
        offsets[n] = len(out)
        out += b"%d 0 obj\n" % n + body + b"\n"
        if stream is not None:
            out += b"stream\n" + stream + b"endstream\n"
        out += b"endobj\n"

    maxnum = max(objs)
    xref_at = len(out)
    out += b"xref\n0 %d\n" % (maxnum + 1)
    out += b"0000000000 65535 f \n"
    for n in range(1, maxnum + 1):
        out += (b"%010d 00000 n \n" % offsets[n]) if n in offsets else b"0000000000 65535 f \n"
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (maxnum + 1, xref_at)
    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: pdf_make_type3.py OUT.pdf")
    with open(sys.argv[1], "wb") as fh:
        fh.write(build())
