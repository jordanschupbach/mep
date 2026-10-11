#!/usr/bin/env python3
"""Author a PDF whose text extracts as nonsense, by hand.

Called by tools/pdf_make_fixtures.sh. It reproduces the shape of The Art
of Computer Programming vol. 4A, which is the file this case was found
in: every font there is a Type 3 bitmap with no /ToUnicode, so the codes
resolve through the font's own encoding to arbitrary letters and a page
of combinatorics extracts as

    C C C C
    D D D B D D D D

Nothing is *missing* -- the glyphs map, they just map to lies -- so no
unmapped-glyph counter can see it, and poppler makes its own different
nonsense of the same file, so there is no oracle to diff against either.
What catches it is diversity: see src/text_quality.h.

The trick here is a /Differences array that sends almost every code to
the same two or three glyphs, which is exactly what the real file's
encodings amount to. The visible page is meaningless, which is the
point: a reader sees typeset text and the extractor sees "D D C".
"""

import sys

# Every code in the printable range mapped to one of three glyphs.
GLYPHS = ["D", "C", "B"]


def build():
    diffs = []
    for code in range(32, 127):
        diffs.append("/%s" % GLYPHS[code % 3])
    encoding = ("<< /Type /Encoding /BaseEncoding /WinAnsiEncoding /Differences [32 "
                + " ".join(diffs) + "] >>").encode()

    lines = []
    y = 740
    for row in range(34):
        # Real-looking line lengths, so the page passes for typeset text.
        text = "The quick brown fox jumps over the lazy dog %d" % row
        lines.append(b"BT /F1 11 Tf 72 %d Td (%s) Tj ET" % (y, text.encode()))
        y -= 20
    content = b"\n".join(lines) + b"\n"

    objs = {}
    objs[1] = (b"<< /Type /Catalog /Pages 2 0 R >>", None)
    objs[2] = (b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>", None)
    objs[3] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
               b" /Resources << /Font << /F1 4 0 R >> >> /Contents 5 0 R >>", None)
    objs[4] = (b"<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica /Encoding 6 0 R >>", None)
    objs[5] = (b"<< /Length %d >>" % len(content), content)
    objs[6] = (encoding, None)

    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    off = {}
    for n in sorted(objs):
        body, stream = objs[n]
        off[n] = len(out)
        out += b"%d 0 obj\n" % n + body + b"\n"
        if stream is not None:
            out += b"stream\n" + stream + b"endstream\n"
        out += b"endobj\n"
    mx = max(objs)
    xr = len(out)
    out += b"xref\n0 %d\n" % (mx + 1) + b"0000000000 65535 f \n"
    for n in range(1, mx + 1):
        out += b"%010d 00000 n \n" % off[n]
    out += b"trailer\n<< /Size %d /Root 1 0 R >>\nstartxref\n%d\n%%%%EOF\n" % (mx + 1, xr)
    return bytes(out)


if __name__ == "__main__":
    if len(sys.argv) != 2:
        sys.exit("usage: pdf_make_degenerate.py OUT.pdf")
    with open(sys.argv[1], "wb") as fh:
        fh.write(build())
