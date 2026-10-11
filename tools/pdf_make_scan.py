#!/usr/bin/env python3
"""Author a PDF that is a high-resolution bitonal scan, by hand.

Called by tools/pdf_make_fixtures.sh. It exists because the interesting
case for image sampling is *minification*: a scanned page is a 600dpi
1-bit stencil -- thousands of pixels across -- drawn onto a page that
renders at 144dpi, so four or five source pixels land on each device
pixel. Reading only one of them (nearest-neighbour) throws the rest
away, which breaks up thin strokes and speckles the result. Measured
against poppler on a real 4205x6000 CCITT scan, averaging the footprint
instead cut the error by 16% and 32% on two pages.

None of the LaTeX-generated fixtures produce that shape, and the real
scans in the author's library cannot be committed, so this stands in for
them: a 2400x3000 ImageMask of thin strokes at a range of angles and
weights, placed on a letter page. Flate-compressed so the fixture stays
small despite the pixel count.

The strokes are deliberately near-horizontal and near-vertical as well
as diagonal -- a thin line almost parallel to the sampling grid is where
point sampling fails most visibly, and is what a scanned fraction bar or
table rule is.
"""

import sys
import zlib

W, H = 2400, 3000


def build_mask():
    """A 1-bit-per-pixel ImageMask: bit 0 = paint (default /Decode [0 1]).
    Rows are byte-padded, per spec 8.9.3."""
    stride = (W + 7) // 8
    rows = bytearray(b"\xff" * (stride * H))  # 1 = don't paint, i.e. blank

    def paint(x, y):
        if 0 <= x < W and 0 <= y < H:
            i = y * stride + (x >> 3)
            rows[i] &= ~(0x80 >> (x & 7)) & 0xFF

    # Horizontal hairlines at a sweep of thicknesses: a scanned rule.
    y = 100
    for t in (1, 1, 2, 2, 3, 4, 6, 8):
        for dy in range(t):
            for x in range(200, W - 200):
                paint(x, y + dy)
        y += 90

    # Near-horizontal lines with a slow rise -- the worst case for point
    # sampling, since the stroke drifts across the pixel grid.
    for k in range(8):
        y0 = 900 + k * 90
        for x in range(200, W - 200):
            paint(x, y0 + (x * (k + 1)) // (W * 2))
            paint(x, y0 + 1 + (x * (k + 1)) // (W * 2))

    # Vertical hairlines, the same sweep rotated.
    x = 200
    for t in (1, 1, 2, 2, 3, 4, 6, 8):
        for dx in range(t):
            for yy in range(1700, 2100):
                paint(x + dx, yy)
        x += 90

    # A dense diagonal hatch, which point sampling turns into moire.
    for d in range(0, W + H, 7):
        for i in range(400):
            paint(d - i, 2200 + i)

    # Text-like strokes: short thin runs at small size, which is what the
    # body text of a scan actually is.
    for row in range(10):
        yy = 2650 + row * 30
        for word in range(30):
            x0 = 200 + word * 70
            for ch in range(5):
                cx = x0 + ch * 12
                for dy in range(14):
                    paint(cx, yy + dy)
                for dx in range(7):
                    paint(cx + dx, yy + 13)
    return bytes(rows)


def build():
    mask = zlib.compress(build_mask(), 9)
    # The whole page: the stencil fills the printable area, so at a 2
    # px/pt render it is minified about 4:1 -- a real scan's ratio.
    # `cm` takes all six matrix operands: the unit square the image
    # occupies is scaled to 504x684pt and translated to the margin.
    content = b"q 0 0 0 rg 504 0 0 684 54 54 cm /Im1 Do Q\n"

    objs = {}
    objs[1] = (b"<< /Type /Catalog /Pages 2 0 R >>", None)
    objs[2] = (b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>", None)
    objs[3] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
               b" /Resources << /XObject << /Im1 4 0 R >> >> /Contents 5 0 R >>", None)
    objs[4] = (b"<< /Type /XObject /Subtype /Image /Width %d /Height %d /ImageMask true"
               b" /Filter /FlateDecode /Length %d >>" % (W, H, len(mask)), mask)
    objs[5] = (b"<< /Length %d >>" % len(content), content)

    out = bytearray(b"%PDF-1.4\n%\xe2\xe3\xcf\xd3\n")
    off = {}
    for n in sorted(objs):
        body, stream = objs[n]
        off[n] = len(out)
        out += b"%d 0 obj\n" % n + body + b"\n"
        if stream is not None:
            out += b"stream\n" + stream + b"\nendstream\n"
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
        sys.exit("usage: pdf_make_scan.py OUT.pdf")
    with open(sys.argv[1], "wb") as fh:
        fh.write(build())
