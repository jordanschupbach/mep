#!/usr/bin/env python3
"""Builds mep-tex's TeX bundle: the part of a TeX Live installation that
mep's own engine needs to typeset LaTeX, packed into one file.

  tex_bundle.py --tree TEXMF [--tree TEXMF...] --out tex-bundle.mtb
                [--manifest packages.json] [--source "TeX Live 2025"]

Each --tree is a texmf root (texmf-dist, texmf-var, ...); a file found in
an earlier tree wins. Kept: the macro files pdfTeX-based LaTeX reads
(tex/latex, tex/generic, tex/plain), font metrics, Type 1 fonts,
encodings, virtual fonts, the pdfTeX map, and the generated configuration
(language.dat, pdftex.map). Left out: documentation, sources, other
engines' files (luatex, xetex, context), and fonts pdfTeX cannot use.

cm-super (58 MB of Type 1 EC fonts) is too big to carry. Instead a
fallback map, mep-fallback.map, draws the EC fonts (ecrm1000 ...) and
the TS1 companions (tcrm1000 ...) with Latin Modern, which descends from
them: mep-tex reads it after pdftex.map and uses a line only for a font
nothing else maps, so a TeX Live with cm-super still uses cm-super.

--manifest is a JSON list of {"name", "version", "licenses"} for the TeX
Live packages the tree was made from (nix/tex-bundle.nix writes it); it
is stored in the bundle as MANIFEST.json, with LICENSES.txt.

The file format ("MEPTEXB1"), read by src/tex/tex_bundle.cpp:
  magic   b"MEPTEXB1\\n"
  u32     file count                      (all integers little-endian)
  per file:
    u16   path length, then the path (UTF-8, relative, '/'-separated)
    u8    method: 0 stored, 1 zlib
    u32   size, u32 stored size, u32 CRC-32 of the contents
    the stored bytes
Files are stored in path order, so the same tree gives the same bundle.
"""

import argparse
import json
import os
import re
import struct
import sys
import zlib

# Subtrees of a texmf root that are kept, and ones excluded inside them.
KEEP = (
    "tex/latex/", "tex/generic/", "tex/plain/",
    "fonts/tfm/", "fonts/type1/", "fonts/enc/", "fonts/vf/",
    "fonts/map/pdftex/",
)
DROP = (
    "fonts/type1/public/cm-super/",     # see the module docstring
    "fonts/map/pdftex/cm-super/",
    "tex/generic/hyph-utf8/patterns/txt/",
    "tex/generic/hyph-utf8/patterns/quote/",
    "tex/generic/hyph-utf8/patterns/ptex/",
    "tex/latex/latex-lab/",             # tagging code: LuaTeX/new-kernel only
)
DROP_SUFFIX = (".dtx", ".ins", ".html", ".md", ".txt~", ".lua", ".luc")

# Generated configuration, wherever it lives in a tree.
CONFIG = ("tex/generic/config/language.dat", "tex/generic/config/language.def",
          "fonts/map/pdftex/updmap/pdftex.map")


def walk(root):
    """Relative paths of the files under root (following symlinks, as nix trees are links)."""
    out = []
    for dirpath, _dirs, files in os.walk(root, followlinks=True):
        for f in files:
            full = os.path.join(dirpath, f)
            rel = os.path.relpath(full, root).replace(os.sep, "/")
            out.append(rel)
    return out


def wanted(rel):
    if rel in CONFIG:
        return True
    if not rel.startswith(KEEP) or rel.startswith(DROP):
        return False
    if rel.endswith(DROP_SUFFIX) or "/doc/" in rel or "/source/" in rel:
        return False
    base = rel.rsplit("/", 1)[-1]
    return not base.lower().startswith(("readme", "changes", "license", "manifest"))


# --- The EC -> Latin Modern fallback map -------------------------------------------------
#
# EC font names: ec<shape><size>, size in hundredths of a point (1000 =
# 10pt). Latin Modern's own T1 fonts are ec-lm<shape><design size>; the
# map line for each is in lm.map. An EC font takes the Latin Modern font
# of its shape at the nearest design size.
EC_SHAPES = {
    # ec shape -> (lm T1 family stem, design sizes)
    "rm": ("ec-lmr", (5, 6, 7, 8, 9, 10, 12, 17)),
    "ti": ("ec-lmri", (7, 8, 9, 10, 12)),
    "bx": ("ec-lmbx", (5, 6, 7, 8, 9, 10, 12)),
    "bi": ("ec-lmbxi", (10,)),
    "sl": ("ec-lmro", (8, 9, 10, 12, 17)),
    "bl": ("ec-lmbxo", (10,)),
    "cc": ("ec-lmcsc", (10,)),
    "tt": ("ec-lmtt", (8, 9, 10, 12)),
    "it": ("ec-lmtti", (10,)),
    "st": ("ec-lmtto", (10,)),
    "tc": ("ec-lmtcsc", (10,)),
    "ss": ("ec-lmss", (8, 9, 10, 12, 17)),
    "si": ("ec-lmsso", (8, 9, 10, 12, 17)),
    "sx": ("ec-lmssbx", (10,)),
    "so": ("ec-lmssbo", (10,)),
    "dh": ("ec-lmssdc", (10,)),
}


def nearest(sizes, pt):
    return min(sizes, key=lambda s: (abs(s - pt), s))


def map_lines(text):
    """(tfm, line) for each font line of a map file."""
    for line in text.splitlines():
        if line.strip() and not line.lstrip().startswith(("%", "#", ";", "*")):
            yield line.split(None, 1)[0], line


def font_files(line):
    """The files a map line names (<file, <<file, <[file)."""
    return [m.group(1) for m in re.finditer(r"<[<\[]?\s*([^\s<]+)", line)]


def filter_map(text, have):
    """pdftex.map without the lines whose font files are not in the bundle."""
    kept, dropped = [], 0
    for line in text.splitlines():
        if line.strip() and not line.lstrip().startswith(("%", "#", ";", "*")):
            if any(f not in have for f in font_files(line)):
                dropped += 1
                continue
        kept.append(line)
    return "\n".join(kept) + "\n", dropped


def fallback_map(files, lm_lines):
    """mep-fallback.map's text: a line for each EC/TC metric whose shape Latin Modern has."""
    lm = {}
    for tfm, line in lm_lines:
        lm[tfm] = line.split(None, 1)[1] if " " in line.strip() else ""
    out = ["% mep-fallback.map -- generated by tools/tex_bundle.py.",
           "% EC and TS1 fonts drawn by Latin Modern, for TeX trees without cm-super;",
           "% mep-tex uses a line only for a font no other map file names."]
    for f in files:
        m = re.fullmatch(r"fonts/tfm/jknappen/ec/(ec|tc)([a-z]{2})(\d{4})\.tfm", f)
        if not m:
            continue
        enc, shape, size = m.group(1), m.group(2), int(m.group(3))
        if shape not in EC_SHAPES:
            continue
        stem, sizes = EC_SHAPES[shape]
        if enc == "tc":
            stem = "ts1" + stem[2:]
        target = "%s%d" % (stem, nearest(sizes, size / 100))
        if lm.get(target):
            out.append("%s%s%04d %s" % (enc, shape, size, lm[target]))
    return "\n".join(out) + "\n"


def licenses_text(manifest, source):
    lines = [
        "The TeX files in this bundle come unmodified from %s." % (source or "TeX Live"),
        "Each keeps its own licence; by TeX Live package:",
        "",
    ]
    for p in sorted(manifest, key=lambda p: p["name"]):
        lic = ", ".join(p.get("licenses") or ["see the package"])
        lines.append("  %-28s %-14s %s" % (p["name"], p.get("version", ""), lic))
    lines += ["", "mep-fallback.map is generated by mep (MIT)."]
    return "\n".join(lines) + "\n"


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--tree", action="append", required=True, help="a texmf root (earlier trees win)")
    ap.add_argument("--out", required=True)
    ap.add_argument("--manifest", help="JSON list of the TeX Live packages the trees hold")
    ap.add_argument("--source", default="", help='where the files come from, e.g. "TeX Live 2025"')
    args = ap.parse_args()

    chosen = {}  # rel -> absolute path
    for root in args.tree:
        for rel in walk(root):
            if rel not in chosen and wanted(rel):
                chosen[rel] = os.path.join(root, rel)

    def read(rel):
        with open(chosen[rel], "rb") as fh:
            return fh.read()

    extra = {}
    # The font files in the bundle, by name, as map files name them.
    have = {rel.rsplit("/", 1)[-1] for rel in chosen}
    pdftex_map = "fonts/map/pdftex/updmap/pdftex.map"
    if pdftex_map in chosen:
        text = read(pdftex_map).decode("latin-1")
        filtered, dropped = filter_map(text, have)
        extra[pdftex_map] = filtered.encode("latin-1")
        if dropped:
            print("tex_bundle: pdftex.map: left out %d fonts whose files are not bundled" % dropped, file=sys.stderr)
        fb = fallback_map(sorted(chosen), list(map_lines(filtered)))
        extra["fonts/map/pdftex/mep/mep-fallback.map"] = fb.encode("latin-1")
    else:
        print("tex_bundle: warning: no fonts/map/pdftex/updmap/pdftex.map in the trees", file=sys.stderr)
    manifest = []
    if args.manifest:
        with open(args.manifest) as fh:
            manifest = json.load(fh)
        extra["MANIFEST.json"] = (json.dumps(manifest, indent=1, sort_keys=True) + "\n").encode()
    extra["LICENSES.txt"] = licenses_text(manifest, args.source).encode()

    paths = sorted(set(chosen) | set(extra))
    raw_total = stored_total = 0
    tmp = args.out + ".tmp"
    with open(tmp, "wb") as out:
        out.write(b"MEPTEXB1\n")
        out.write(struct.pack("<I", len(paths)))
        for rel in paths:
            data = extra[rel] if rel in extra else read(rel)
            packed = zlib.compress(data, 9)
            method, body = (1, packed) if len(packed) < len(data) else (0, data)
            name = rel.encode()
            out.write(struct.pack("<H", len(name)) + name)
            out.write(struct.pack("<BIII", method, len(data), len(body), zlib.crc32(data) & 0xFFFFFFFF))
            out.write(body)
            raw_total += len(data)
            stored_total += len(body)
    os.replace(tmp, args.out)
    print("tex_bundle: %d files, %.1f MB -> %.1f MB in %s" %
          (len(paths), raw_total / 1e6, stored_total / 1e6, args.out), file=sys.stderr)


if __name__ == "__main__":
    main()
