#!/usr/bin/env python3
"""Audit the Unicode coverage of mep's embedded font headers.

mep embeds every font it uses as a C byte array in src/*_font_data.h /
src/font_data.h, so the usual on-disk tools (fc-query) can't answer "does
this font actually contain codepoint X?" without first reconstructing the
TTF. This script parses the hex bytes straight out of a header, walks the
sfnt table directory and cmap (formats 4 and 12 -- the only ones
src/gfx/truetype.cpp reads), and reports coverage. Stdlib only; no
fontTools needed.

Why you care: the renderer's GetGlyphIndex falls back to '?' (or atlas
glyph 0) for a codepoint missing from a font's *bake list*, and a
codepoint on the bake list but missing from the font's cmap silently
bakes nothing -- so whenever a kXxxCodepointRanges table in main.cpp or a
pyftsubset regen command in a font header changes, run this to confirm
the font really has what the ranges promise.

Usage:
  tools/font_coverage.py --check U+2744,U+1F995 src/*_font_data.h src/font_data.h
      Per-font YES/no matrix for the given codepoints.
  tools/font_coverage.py --sfnt src/emoji_font_data.h
      Print each font's sfnt flavor (glyf vs CFF/'OTTO') and table list.
      src/gfx/truetype.cpp renders glyf ONLY -- a CFF-flavored subset
      would silently draw nothing; check this after every regen.
  tools/font_coverage.py --diff-ranges "U+1F300-1F5FF,U+2600-27BF" src/emoji_font_data.h
      Codepoints inside the given ranges that the font does NOT cover
      (for trimming a ranges table down to reality), plus covered count.
"""

import argparse
import re
import struct
import sys


def parse_header_bytes(path):
    """Extract the first C byte-array literal from a header as bytes."""
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    # Everything between the array's opening and closing brace; headers
    # here hold one big array (plus a _len constant the regex ignores).
    m = re.search(r"\[\]\s*=\s*\{(.*?)\}", text, re.S)
    if not m:
        raise ValueError(f"{path}: no byte-array literal found")
    return bytes(int(tok, 0) for tok in re.findall(r"0[xX][0-9a-fA-F]+|\d+", m.group(1)))


def sfnt_tables(data):
    """Return (flavor, {tag: (offset, length)}) from an sfnt table directory."""
    if len(data) < 12:
        raise ValueError("too short for an sfnt header")
    (version, num_tables) = struct.unpack(">IH", data[:6])
    flavor = {0x00010000: "glyf (TrueType outlines)", 0x4F54544F: "CFF ('OTTO' -- NOT renderable by src/gfx/truetype.cpp)",
              0x74727565: "glyf ('true' -- Apple)"}.get(version, f"unknown 0x{version:08X}")
    tables = {}
    for i in range(num_tables):
        rec = data[12 + 16 * i: 28 + 16 * i]
        tag, _checksum, offset, length = struct.unpack(">4sIII", rec)
        tables[tag.decode("latin-1")] = (offset, length)
    return flavor, tables


def cmap_codepoints(data):
    """Set of codepoints mapped by the font's cmap (formats 4 and 12 only,
    mirroring src/gfx/truetype.cpp)."""
    _flavor, tables = sfnt_tables(data)
    if "cmap" not in tables:
        raise ValueError("no cmap table")
    off, _length = tables["cmap"]
    (_ver, num_sub) = struct.unpack(">HH", data[off:off + 4])
    covered = set()
    for i in range(num_sub):
        p_id, e_id, sub_off = struct.unpack(">HHI", data[off + 4 + 8 * i: off + 12 + 8 * i])
        # Unicode-capable subtables only (same platforms truetype.cpp accepts).
        if not (p_id == 0 or (p_id == 3 and e_id in (1, 10))):
            continue
        sub = off + sub_off
        (fmt,) = struct.unpack(">H", data[sub:sub + 2])
        if fmt == 4:
            (seg_x2,) = struct.unpack(">H", data[sub + 6:sub + 8])
            segs = seg_x2 // 2
            ends = struct.unpack(f">{segs}H", data[sub + 14: sub + 14 + seg_x2])
            starts_off = sub + 16 + seg_x2
            starts = struct.unpack(f">{segs}H", data[starts_off: starts_off + seg_x2])
            for s, e in zip(starts, ends):
                if s == 0xFFFF:
                    continue
                covered.update(range(s, e + 1))
        elif fmt == 12:
            (n_groups,) = struct.unpack(">I", data[sub + 12:sub + 16])
            for g in range(n_groups):
                s, e, _gid = struct.unpack(">III", data[sub + 16 + 12 * g: sub + 28 + 12 * g])
                covered.update(range(s, e + 1))
    return covered


def parse_cp(tok):
    tok = tok.strip().upper().replace("U+", "")
    return int(tok, 16)


def parse_ranges(spec):
    """"U+1F300-1F5FF,U+2744" -> list of (lo, hi)."""
    out = []
    for part in spec.split(","):
        if "-" in part.strip().lstrip("U+u+"):
            lo, hi = part.split("-")
            out.append((parse_cp(lo), parse_cp(hi)))
        else:
            cp = parse_cp(part)
            out.append((cp, cp))
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--check", help="comma-separated codepoints (U+XXXX) to test per font")
    ap.add_argument("--sfnt", action="store_true", help="print sfnt flavor + tables per font")
    ap.add_argument("--diff-ranges", help="ranges spec; report uncovered codepoints per font")
    ap.add_argument("headers", nargs="+", help="src/*_font_data.h paths")
    args = ap.parse_args()
    if not (args.check or args.sfnt or args.diff_ranges):
        ap.error("pick at least one of --check / --sfnt / --diff-ranges")

    for path in args.headers:
        try:
            data = parse_header_bytes(path)
        except ValueError as err:
            print(f"{path}: SKIP ({err})")
            continue
        name = path.split("/")[-1]
        if args.sfnt:
            flavor, tables = sfnt_tables(data)
            print(f"{name}: {len(data)} bytes, sfnt {flavor}")
            print(f"  tables: {' '.join(sorted(tables))}")
        if args.check or args.diff_ranges:
            covered = cmap_codepoints(data)
            if args.check:
                print(f"{name} ({len(covered)} codepoints mapped):")
                for tok in args.check.split(","):
                    cp = parse_cp(tok)
                    label = chr(cp) if 0x20 <= cp else ""
                    print(f"  U+{cp:05X} {label}\t{'YES' if cp in covered else 'no'}")
            if args.diff_ranges:
                missing, total = [], 0
                for lo, hi in parse_ranges(args.diff_ranges):
                    for cp in range(lo, hi + 1):
                        total += 1
                        if cp not in covered:
                            missing.append(cp)
                print(f"{name}: {total - len(missing)}/{total} of the given ranges covered")
                # Compact the missing list back into ranges for readability.
                i = 0
                while i < len(missing):
                    j = i
                    while j + 1 < len(missing) and missing[j + 1] == missing[j] + 1:
                        j += 1
                    span = f"U+{missing[i]:05X}" if i == j else f"U+{missing[i]:05X}-U+{missing[j]:05X}"
                    print(f"  missing {span}")
                    i = j + 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
