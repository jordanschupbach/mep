#!/usr/bin/env python3
"""Measure mep's PDF text extraction and page rendering against poppler.

mep has its own from-scratch PDF stack (src/pdf_*.cpp, no poppler/mupdf/
pdfium), and every pdf_*_test.cpp exercises it with hand-assembled byte
strings. Those tests say the parser is right about the bytes they feed
it; they say nothing about how the stack does on a real typeset
document. This closes that gap by diffing mep against an independent
implementation over whole files.

Ground truth is poppler, which is already a dev-shell dependency
(flake.nix pulls poppler-utils in as a fixture generator):

  text   `pdftotext`  vs  `mep-pdf-probe text`
  render `pdftoppm`   vs  `mep-pdf-probe render`, compared with ImageMagick

Neither tool is "correct" in an absolute sense -- extraction from a PDF
is inference, not a lookup, and the two disagree on blank lines and
column order even where both are right. So the metrics here are built to
be robust to formatting disagreement and sensitive to *missing content*:
a word-level F1 after aggressive normalization, plus a `missing_rate`
that counts ground-truth words mep never produced. The second one is the
real signal -- a glyph whose font gives no usable Unicode is dropped
from mep's text layer outright (pdf_content.cpp only keeps a TextGlyph
when GetUnicodeText returned something), so it shows up here as a word
poppler found and mep didn't.

Usage:
  tools/pdf_accuracy.py text   --build-dir build/native-dev FILE.pdf ...
  tools/pdf_accuracy.py text   --build-dir build/native-dev --corpus ~/books --limit 200
  tools/pdf_accuracy.py render --build-dir build/native-dev --scale 2 FILE.pdf
  tools/pdf_accuracy.py text   --baseline out.json FILE.pdf     # record
  tools/pdf_accuracy.py text   --compare  out.json FILE.pdf     # diff vs recorded

The corpus sweep is opt-in and never reads anything unless asked: this
is run against the user's own library, which does not belong in the
repo or in a default code path.
"""

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import tempfile
import unicodedata

# ---------------------------------------------------------------- helpers


def run(cmd, timeout=180):
    """Run a command, returning (rc, stdout, stderr) and never raising."""
    try:
        p = subprocess.run(cmd, capture_output=True, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace"), p.stderr.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return -9, "", "timeout after %ss" % timeout
    except OSError as e:
        return -1, "", str(e)


def need(tool):
    if shutil.which(tool) is None:
        sys.exit("required tool not found on PATH: %s" % tool)


# Normalization shared by both sides before any comparison. The goal is
# to erase the differences the two tools are *entitled* to disagree
# about (line wrapping, how many blank lines a column break is worth,
# ligature spelling, hyphenation at a line end, which dash or quote
# character got used) while preserving every actual word.
_DASHES = dict.fromkeys(map(ord, "‐‑‒–—―−"), "-")
_QUOTES = dict.fromkeys(map(ord, "‘’‚‛′"), "'")
_DQUOTES = dict.fromkeys(map(ord, "“”„‟″"), '"')


def normalize(s):
    # NFKC folds ligatures (ﬁ -> fi) and compatibility forms, which is
    # exactly the disagreement we don't care about.
    s = unicodedata.normalize("NFKC", s)
    s = s.translate(_DASHES).translate(_QUOTES).translate(_DQUOTES)
    # A word broken over a line end: join it back before whitespace
    # collapsing, or the two halves become two bogus words.
    s = re.sub(r"(\w)-\n(\w)", r"\1\2", s)
    s = s.replace("­", "")  # soft hyphen
    return s


def words(s):
    """Comparable word tokens. Punctuation is dropped on both sides: it is
    the single biggest source of harmless disagreement, and a missing
    word is what we are hunting."""
    s = normalize(s).lower()
    return [w for w in re.findall(r"[^\W_]+", s, flags=re.UNICODE) if w]


def multiset_f1(ref, hyp):
    """Word-level precision/recall/F1 over multisets, plus the fraction of
    reference words absent from the hypothesis. Multiset rather than set
    so a page that drops three of five 'the's is scored for it."""
    from collections import Counter

    rc, hc = Counter(ref), Counter(hyp)
    overlap = sum((rc & hc).values())
    nref, nhyp = sum(rc.values()), sum(hc.values())
    prec = overlap / nhyp if nhyp else (1.0 if not nref else 0.0)
    rec = overlap / nref if nref else 1.0
    f1 = 2 * prec * rec / (prec + rec) if (prec + rec) else 0.0
    missing = (nref - overlap) / nref if nref else 0.0
    return {
        "precision": prec,
        "recall": rec,
        "f1": f1,
        "missing_rate": missing,
        "ref_words": nref,
        "hyp_words": nhyp,
        # Words poppler saw that mep did not, worst first -- the
        # actionable half of the metric.
        "missing_sample": [w for w, n in (rc - hc).most_common(12)],
    }


# ------------------------------------------------------------------ text


def page_count(probe, path):
    rc, out, _ = run([probe, "info", path])
    if rc != 0:
        return 0
    for line in out.splitlines():
        f = line.split("\t")
        if f and f[0] == "pages":
            return int(f[1])
    return 0


def mep_text(probe, path, page):
    rc, out, err = run([probe, "text", path, "--page", str(page)])
    if rc != 0:
        return None, err.strip()[:200]
    return out, None


def poppler_text(path, page):
    # -layout keeps reading order closer to the visual page, which is
    # what mep's own visual-order extraction is aiming at too.
    rc, out, err = run(["pdftotext", "-layout", "-f", str(page + 1), "-l", str(page + 1), path, "-"])
    if rc != 0:
        return None, err.strip()[:200]
    return out, None


def cmd_text(args):
    need("pdftotext")
    probe = os.path.join(args.build_dir, "mep-pdf-probe")
    if not os.path.exists(probe):
        sys.exit("no mep-pdf-probe at %s (cmake --build %s --target mep-pdf-probe)" % (probe, args.build_dir))

    files = collect_files(args)
    if not files:
        sys.exit("no PDFs to measure")

    per_file = []
    for path in files:
        n = page_count(probe, path)
        if n == 0:
            per_file.append({"file": path, "error": "mep could not parse"})
            print("%-58s  UNPARSEABLE" % shorten(path))
            continue
        pages = pick_pages(n, args.pages)
        rows = []
        for pg in pages:
            mt, e1 = mep_text(probe, path, pg)
            pt, e2 = poppler_text(path, pg)
            if mt is None or pt is None:
                rows.append({"page": pg, "error": e1 or e2})
                continue
            ref_w, hyp_w = words(pt), words(mt)
            # A page with no reference text at all is a scanned image,
            # not a page mep got wrong: poppler finds nothing either, so
            # there is nothing to be accurate about. Scoring it 0 would
            # bury the real failures under the library's scanned books.
            if not ref_w:
                rows.append({"page": pg, "skipped": "no text layer", "hyp_words": len(hyp_w)})
                continue
            m = multiset_f1(ref_w, hyp_w)
            m["page"] = pg
            rows.append(m)
        ok = [r for r in rows if "f1" in r]
        n_skipped = sum(1 for r in rows if "skipped" in r)
        if not ok:
            per_file.append({"file": path, "pages": rows,
                             "error": "no text layer (scanned?)" if n_skipped else "no comparable pages"})
            print("%-58s  SKIPPED (%s)" % (shorten(path),
                                           "no text layer" if n_skipped else "no comparable pages"))
            continue
        # Weight by reference length: a title page with four words should
        # not count as much as a dense page of prose.
        tot_ref = sum(r["ref_words"] for r in ok) or 1
        summary = {
            "file": path,
            "n_pages_measured": len(ok),
            "f1": sum(r["f1"] * r["ref_words"] for r in ok) / tot_ref,
            "missing_rate": sum(r["missing_rate"] * r["ref_words"] for r in ok) / tot_ref,
            "ref_words": tot_ref,
            "n_pages_skipped": n_skipped,
            "worst": sorted(ok, key=lambda r: r["f1"])[:3],
            "pages": ok if args.verbose else None,
        }
        per_file.append(summary)
        print(
            "%-58s  f1=%.3f  missing=%.3f  (%d pages, %d ref words)"
            % (shorten(path), summary["f1"], summary["missing_rate"], summary["n_pages_measured"], tot_ref)
        )
        if args.verbose:
            for w in summary["worst"]:
                print(
                    "      page %-4d f1=%.3f missing=%.3f  absent: %s"
                    % (w["page"], w["f1"], w["missing_rate"], ", ".join(w["missing_sample"][:8]))
                )

    report(per_file, args, kind="text")


# ---------------------------------------------------------------- render


def cmd_render(args):
    need("pdftoppm")
    need("magick")
    probe = os.path.join(args.build_dir, "mep-pdf-probe")
    if not os.path.exists(probe):
        sys.exit("no mep-pdf-probe at %s" % probe)

    files = collect_files(args)
    per_file = []
    with tempfile.TemporaryDirectory(prefix="pdfacc-") as tmp:
        for path in files:
            n = page_count(probe, path)
            if n == 0:
                per_file.append({"file": path, "error": "mep could not parse"})
                continue
            rows = []
            for pg in pick_pages(n, args.pages):
                mine = os.path.join(tmp, "mep.png")
                rc, _, err = run([probe, "render", path, "--page", str(pg), "--scale", str(args.scale), "--out", mine])
                if rc != 0:
                    rows.append({"page": pg, "error": err.strip()[:160]})
                    continue
                # pdftoppm names its output <root>-<n>.png; -r takes DPI,
                # and mep's scale is px per point, so 72*scale.
                root = os.path.join(tmp, "ref")
                rc, _, err = run(
                    [
                        "pdftoppm", "-png", "-r", str(int(round(72 * args.scale))),
                        "-f", str(pg + 1), "-l", str(pg + 1), "-singlefile", path, root,
                    ]
                )
                ref = root + ".png"
                if rc != 0 or not os.path.exists(ref):
                    rows.append({"page": pg, "error": "pdftoppm failed: " + err.strip()[:140]})
                    continue
                m = compare_images(ref, mine, tmp)
                if m is None:
                    rows.append({"page": pg, "error": "compare failed"})
                    continue
                m["page"] = pg
                rows.append(m)
            ok = [r for r in rows if "rmse" in r]
            if not ok:
                per_file.append({"file": path, "error": "no comparable pages", "pages": rows})
                continue
            summary = {
                "file": path,
                "n_pages_measured": len(ok),
                "rmse": statistics.fmean(r["rmse"] for r in ok),
                "diff_frac": statistics.fmean(r["diff_frac"] for r in ok),
                "ink": statistics.fmean(r.get("ink", 0.0) for r in ok),
                "rmse_per_ink": statistics.fmean(r.get("rmse_per_ink", 0.0) for r in ok),
                "worst": sorted(ok, key=lambda r: -r["rmse"])[:3],
                "pages": ok if args.verbose else None,
            }
            per_file.append(summary)
            print(
                "%-50s  rmse=%.4f  rmse/ink=%6.2f  (%d pages)"
                % (shorten(path, 50), summary["rmse"], summary["rmse_per_ink"], summary["n_pages_measured"])
            )
    report(per_file, args, kind="render")


# Below this much ink, rmse/ink is division by noise rather than a
# measurement: a page that is almost entirely blank has a denominator
# small enough that any disagreement at all dominates.
kMinRankInk = 0.004


def compare_images(ref, mine, tmp):
    """RMSE (0..1), the fraction of pixels differing beyond a small
    tolerance, and both of those divided by how much ink the page has.

    The two renderers will never be pixel-identical -- they antialias
    differently -- so diff_frac uses a fuzz, and RMSE is a trend number
    rather than an absolute.

    The normalized pair exists because raw RMSE is not comparable across
    pages. It averages over every pixel, and a mostly-blank page is
    mostly pixels that agree perfectly, which drags its score down for
    reasons that have nothing to do with rendering quality. A dense
    two-column page scored 0.10 against 0.037 for a sparse one and
    looked like the worst fixture in the set -- while per unit of ink it
    was the *best* agreeing page measured (1.70 against 3.5-6.7). Rank
    by `rmse_per_ink`; read `rmse` only against the same page's own
    history."""
    # Normalize geometry first: a one-pixel rounding difference in page
    # size would otherwise dominate everything.
    norm = os.path.join(tmp, "mine_n.png")
    rc, out, _ = run(["magick", "identify", "-format", "%wx%h", ref])
    if rc != 0:
        return None
    rc, _, _ = run(["magick", mine, "-resize", out + "!", "-colorspace", "sRGB", norm])
    if rc != 0:
        return None
    ref_n = os.path.join(tmp, "ref_n.png")
    rc, _, _ = run(["magick", ref, "-colorspace", "sRGB", ref_n])
    if rc != 0:
        return None
    # `compare -metric RMSE` writes "value (normalized)" to stderr and
    # exits non-zero whenever the images differ at all, so rc is not an
    # error signal here.
    _, _, err = run(["magick", "compare", "-metric", "RMSE", ref_n, norm, "null:"])
    m = re.search(r"\(([0-9.eE+-]+)\)", err)
    if not m:
        return None
    rmse = float(m.group(1))
    _, _, err2 = run(["magick", "compare", "-metric", "AE", "-fuzz", "12%", ref_n, norm, "null:"])
    m2 = re.search(r"^\s*([0-9.eE+-]+)", err2)
    w, h = (int(x) for x in out.split("x"))
    diff_frac = (float(m2.group(1)) / (w * h)) if (m2 and w and h) else float("nan")
    # How much of the reference page is ink at all, as 1 - mean luma.
    _, ink_s, _ = run(["magick", ref_n, "-colorspace", "Gray", "-format", "%[fx:1-mean]", "info:"])
    try:
        ink = max(float(ink_s.strip()), 1e-6)
    except ValueError:
        ink = 1e-6
    return {"rmse": rmse, "diff_frac": diff_frac, "ink": ink,
            "rmse_per_ink": rmse / ink, "diff_per_ink": diff_frac / ink}


# ------------------------------------------------------------- plumbing


def shorten(path, width=58):
    base = os.path.basename(path)
    return base if len(base) <= width else base[: width - 3] + "..."


def pick_pages(n, spec):
    """Which pages to measure. Default samples across the document rather
    than taking the first few: front matter is unrepresentatively easy,
    and the math lives in the middle."""
    if spec:
        out = []
        for part in spec.split(","):
            if "-" in part:
                a, b = part.split("-", 1)
                out.extend(range(int(a), min(int(b) + 1, n)))
            else:
                if int(part) < n:
                    out.append(int(part))
        return sorted(set(out))
    if n <= 8:
        return list(range(n))
    step = n / 8.0
    return sorted({int(i * step) for i in range(8)})


def collect_files(args):
    files = list(args.files)
    if args.corpus:
        for root, _, names in os.walk(os.path.expanduser(args.corpus)):
            for nm in names:
                if nm.lower().endswith(".pdf"):
                    files.append(os.path.join(root, nm))
        files.sort()
    if args.limit:
        files = files[: args.limit]
    return files


def report(per_file, args, kind):
    scored = [f for f in per_file if "error" not in f]
    print()
    if not scored:
        print("nothing comparable")
        return
    if kind == "text":
        tot = sum(f["ref_words"] for f in scored) or 1
        agg = {
            "f1": sum(f["f1"] * f["ref_words"] for f in scored) / tot,
            "missing_rate": sum(f["missing_rate"] * f["ref_words"] for f in scored) / tot,
        }
        print("OVERALL  files=%d  f1=%.4f  missing_rate=%.4f" % (len(scored), agg["f1"], agg["missing_rate"]))
        print("worst files by f1:")
        for f in sorted(scored, key=lambda f: f["f1"])[:10]:
            print("   %-56s f1=%.3f missing=%.3f" % (shorten(f["file"], 56), f["f1"], f["missing_rate"]))
    else:
        agg = {
            "rmse": statistics.fmean(f["rmse"] for f in scored),
            "diff_frac": statistics.fmean(f["diff_frac"] for f in scored),
            "rmse_per_ink": statistics.fmean(f.get("rmse_per_ink", 0.0) for f in scored),
        }
        print("OVERALL  files=%d  rmse=%.4f  diff_frac=%.4f  rmse/ink=%.2f"
              % (len(scored), agg["rmse"], agg["diff_frac"], agg["rmse_per_ink"]))
        # Ranked by the density-normalized figure: raw RMSE just finds
        # the densest pages. Pages with almost no ink are listed but not
        # ranked -- dividing by a near-zero denominator is noise, and a
        # nearly blank page topped this list at 8.24 while rendering
        # pixel-for-pixel correctly.
        ranked = [f for f in scored if f.get("ink", 0.0) >= kMinRankInk]
        sparse = [f for f in scored if f.get("ink", 0.0) < kMinRankInk]
        print("worst files by rmse/ink (raw rmse only ranks by how much text a page has):")
        for f in sorted(ranked, key=lambda f: -f.get("rmse_per_ink", 0.0))[:10]:
            print("   %-48s rmse/ink=%6.2f  rmse=%.4f ink=%.4f"
                  % (shorten(f["file"], 48), f.get("rmse_per_ink", 0.0), f["rmse"], f.get("ink", 0.0)))
        for f in sparse:
            print("   %-48s (too little ink to rank: %.4f)" % (shorten(f["file"], 48), f.get("ink", 0.0)))

    blob = {"kind": kind, "aggregate": agg, "files": per_file}
    if args.baseline:
        with open(args.baseline, "w") as fh:
            json.dump(blob, fh, indent=1)
        print("\nbaseline written to %s" % args.baseline)
    if args.compare:
        with open(args.compare) as fh:
            old = json.load(fh)
        print("\n%-16s %12s %12s %10s" % ("metric", "baseline", "now", "delta"))
        for k, v in agg.items():
            o = old.get("aggregate", {}).get(k)
            if o is None:
                continue
            print("%-16s %12.4f %12.4f %+10.4f" % (k, o, v, v - o))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("mode", choices=["text", "render"])
    ap.add_argument("files", nargs="*")
    ap.add_argument("--build-dir", default="build/native-dev")
    ap.add_argument("--corpus", default="", help="walk this directory for PDFs (opt-in; nothing is read without it)")
    ap.add_argument("--limit", type=int, default=0)
    ap.add_argument("--pages", default="", help="e.g. 0,5,10-12; default samples 8 across the document")
    ap.add_argument("--scale", type=float, default=2.0, help="render: px per point (2.0 == 144dpi)")
    ap.add_argument("--baseline", default="", help="write results here as JSON")
    ap.add_argument("--compare", default="", help="diff against a JSON written by --baseline")
    ap.add_argument("-v", "--verbose", action="store_true")
    args = ap.parse_args()
    if args.mode == "text":
        cmd_text(args)
    else:
        cmd_render(args)


if __name__ == "__main__":
    main()
