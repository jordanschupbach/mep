#!/usr/bin/env python3
"""Author a tagged PDF whose list carries /ActualText, by hand.

Called by tools/pdf_make_fixtures.sh. Separate because no LaTeX engine
emits this shape: a marked-content sequence with an /ActualText entry
wrapped around a whole list item, which is what real tagged-PDF
producers (Word, InDesign, accessibility remediation tools) do when the
list marker is not real text.

It is the fixture for a specific suspected failure: pdf_content.cpp
gives every glyph the innermost non-empty /ActualText of the entire open
marked-content stack, and pdf_text.cpp then suppresses every glyph whose
/ActualText equals the previous one's as a repeat. Together those mean
one BDC around many words collapses to that one string -- so a
highlighted list item could reach an AI prompt as nothing but its
bullet, which is exactly the reported symptom.

Three list items, each wrapped differently, so the fixture separates the
cases:
  item 1: /ActualText on the marker only (the correct, narrow use)
  item 2: /ActualText around the marker AND the body text (the wide span
          that should NOT erase the body)
  item 3: no /ActualText at all (the control)
"""

import sys


def build():
    # Content stream. BDC/EMC per spec 14.6; /ActualText per 14.9.4.
    # Each Tj is one word so the glyph boxes come out spaced like real
    # typeset text rather than one run.
    def line(y, words, actual=None, marker="\\267"):
        out = []
        if actual is not None:
            out.append(b"/Span << /ActualText (%s) >> BDC" % actual.encode())
        out.append(b"BT /F1 12 Tf 72 %d Td (%s) Tj ET" % (y, marker.encode()))
        x = 90
        for w in words:
            out.append(b"BT /F1 12 Tf %d %d Td (%s) Tj ET" % (x, y, w.encode()))
            x += 7 * len(w) + 4
        if actual is not None:
            out.append(b"EMC")
        return out

    parts = [b"BT /F1 14 Tf 72 740 Td (Tagged list with ActualText) Tj ET"]
    # Item 1: /ActualText scoped to the marker alone.
    parts += [b"/Span << /ActualText (\\267) >> BDC",
              b"BT /F1 12 Tf 72 700 Td (\\267) Tj ET",
              b"EMC"]
    parts += line(700, ["First", "item", "body", "text", "is", "not", "inside", "the", "span"], actual=None,
                  marker=" ")
    # Item 2: /ActualText spanning the marker and the whole body -- the
    # case that must not collapse to just the bullet.
    parts += line(670, ["Second", "item", "body", "text", "sits", "inside", "one", "wide", "span"],
                  actual="\\267")
    # Item 3: control, no marked content at all.
    parts += line(640, ["Third", "item", "body", "text", "has", "no", "actual", "text", "at", "all"], actual=None)
    content = b"\n".join(parts) + b"\n"

    objs = {}
    objs[1] = (b"<< /Type /Catalog /Pages 2 0 R /StructTreeRoot 7 0 R /MarkInfo << /Marked true >> >>", None)
    objs[2] = (b"<< /Type /Pages /Kids [3 0 R] /Count 1 >>", None)
    objs[3] = (b"<< /Type /Page /Parent 2 0 R /MediaBox [0 0 612 792]"
               b" /Resources << /Font << /F1 5 0 R >> >> /Contents 4 0 R /StructParents 0 >>", None)
    objs[4] = (b"<< /Length %d >>" % len(content), content)
    objs[5] = (b"<< /Type /Font /Subtype /Type1 /BaseFont /Times-Roman /Encoding /WinAnsiEncoding >>", None)
    # A minimal structure tree marking the three items as a list, so the
    # fixture also exercises the L/LI/Lbl/LBody roles a11y_pdf.cpp knows.
    objs[7] = (b"<< /Type /StructTreeRoot /K [8 0 R] >>", None)
    objs[8] = (b"<< /Type /StructElem /S /L /P 7 0 R /K [9 0 R] >>", None)
    objs[9] = (b"<< /Type /StructElem /S /LI /P 8 0 R /Pg 3 0 R /K [] >>", None)

    out = bytearray(b"%PDF-1.5\n%\xe2\xe3\xcf\xd3\n")
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
        sys.exit("usage: pdf_make_tagged.py OUT.pdf")
    with open(sys.argv[1], "wb") as fh:
        fh.write(build())
