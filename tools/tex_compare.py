#!/usr/bin/env python3
"""Compares mep's own TeX engine with a reference pdflatex on a corpus.

  tex_compare.py --ref-pdflatex PATH [--out DIR] [--jobs N] [--fragments]
                 [--mep-tex PATH] [--mep-mepml PATH] SOURCE...

Each SOURCE is a .tex, .org or .mepml file, or a directory searched for
them. An .org or .mepml file is exported to LaTeX by mep-mepml (as an
article, and as a Beamer deck too when it has \\slide blocks); with
--fragments every maths fragment in it is also typeset alone, in the
preamble mep's inline previews use. Each document is then compiled by
`mep-tex compile` (the bundle) and by the reference pdflatex (run until
its auxiliary files settle, like latexmk), and the two PDFs compared:
page count and size, the pages rasterized by poppler (pdftoppm) and
compared pixel by pixel, and their text (pdftotext; the characters, in
any order).

The reference should have the bundle's packages and cm-super (the fonts
the bundle draws with Latin Modern instead):
  nix build --impure --expr '(import ./nix/tex-bundle.nix {...}).reference'

Writes DIR/report.md (worst first) and keeps each document's files in
DIR/<n>/{mine,ref}/ for a look. Exit status 0 when nothing failed in mep
that worked in the reference.
"""

import argparse
import concurrent.futures as cf
import hashlib
import os
import re
import shutil
import subprocess
import sys
import time

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

# What mep's inline maths previews compile (MEP_ORG_LATEX_PREAMBLE, src/main.cpp).
PREVIEW_PREAMBLE = ("\\documentclass[11pt,preview,border=1pt,varwidth=\\maxdimen]{standalone}\n"
                    "\\usepackage{amsmath}\n\\usepackage{amssymb}\n\\begin{document}\n")

ENV_NAMES = r"(?:equation|align|gather|multline|eqnarray|displaymath|alignat|flalign)\*?"
DISPLAY_RE = re.compile(r"\\\[(.+?)\\\]|\$\$(.+?)\$\$|(\\begin\{(" + ENV_NAMES + r")\}.+?\\end\{\4\})", re.S)
INLINE_RE = re.compile(r"(?<![\\$\w])\$(?![\s$])([^$\n]+?)(?<![\s\\])\$(?![\w$])")


def env_clean():
    """The environment for the engines: no LD_LIBRARY_PATH (the dev shell's
    breaks mep's binaries), no inherited TeX tree choices."""
    e = dict(os.environ)
    for k in ("LD_LIBRARY_PATH", "MEP_TEX_TREES", "MEP_TEX_PACKAGES", "TEXINPUTS"):
        e.pop(k, None)
    return e


def run(argv, cwd=None, env=None, timeout=300):
    try:
        p = subprocess.run(argv, cwd=cwd, env=env or env_clean(), stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, stdin=subprocess.DEVNULL, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace")
    except subprocess.TimeoutExpired:
        return 124, "timed out"


def first_error(text):
    for line in text.splitlines():
        if line.startswith("! ") or line.startswith("mep-tex: error") or "Fatal error" in line:
            return line.strip()[:160]
    return ""


def collect(sources):
    out = []
    for s in sources:
        if os.path.isdir(s):
            for dirpath, dirs, files in os.walk(s):
                dirs[:] = [d for d in dirs if d not in ("build", ".git", "node_modules")]
                for f in sorted(files):
                    if f.endswith((".tex", ".org", ".mepml")):
                        out.append(os.path.join(dirpath, f))
        elif os.path.isfile(s):
            out.append(s)
    return sorted(set(os.path.abspath(p) for p in out))


def fragments(path):
    """Maths fragments of an org/mepml file, as preview bodies."""
    try:
        text = open(path, encoding="utf-8", errors="replace").read()
    except OSError:
        return []
    found = []
    for m in DISPLAY_RE.finditer(text):
        if m.group(3):
            found.append(m.group(3))
        else:
            found.append("\\[" + (m.group(1) or m.group(2)) + "\\]")
    rest = DISPLAY_RE.sub(" ", text)
    for m in INLINE_RE.finditer(rest):
        found.append("$" + m.group(1) + "$")
    return found


def raster_diff(a, b, workdir):
    """(pages_a, pages_b, worst fraction of differing pixels, per-page list)."""
    pa, pb = os.path.join(workdir, "ra"), os.path.join(workdir, "rb")
    for pdf, prefix in ((a, pa), (b, pb)):
        run(["pdftoppm", "-r", "40", "-gray", pdf, prefix])
    def pages(prefix):
        d = os.path.dirname(prefix)
        base = os.path.basename(prefix)
        return sorted(f for f in os.listdir(d) if f.startswith(base + "-") and f.endswith(".pgm"))
    la, lb = pages(pa), pages(pb)
    worst, per = 0.0, []
    for fa, fb in zip(la, lb):
        da = open(os.path.join(workdir, fa), "rb").read()
        db = open(os.path.join(workdir, fb), "rb").read()
        ha, hb = da.split(b"\n", 3), db.split(b"\n", 3)
        if ha[1] != hb[1]:
            per.append(1.0)
            worst = 1.0
            continue
        xa, xb = ha[3], hb[3]
        n = sum(1 for x, y in zip(xa, xb) if abs(x - y) > 64)
        frac = n / max(1, len(xa))
        per.append(frac)
        worst = max(worst, frac)
    for f in la + lb:
        os.remove(os.path.join(workdir, f))
    return len(la), len(lb), worst, per


def pdf_text(pdf):
    """The page text's characters, sorted: what a reader would extract,
    without pdftotext's spacing and ordering guesses (which a fraction of a
    point's difference in a glyph's position can change)."""
    code, out = run(["pdftotext", "-q", pdf, "-"])
    return "".join(sorted(re.sub(r"\s+", "", out))) if code == 0 else ""


def aux_state(base):
    h = hashlib.md5()
    for ext in (".aux", ".toc", ".lof", ".lot", ".out", ".nav", ".snm"):
        try:
            h.update(open(base + ext, "rb").read())
        except OSError:
            pass
    return h.hexdigest()


def compile_ref(pdflatex, tex, outdir, srcdir):
    env = env_clean()
    env["TEXINPUTS"] = srcdir + ":"
    base = os.path.join(outdir, os.path.splitext(os.path.basename(tex))[0])
    log = ""
    for _ in range(4):
        before = aux_state(base)
        code, log = run([pdflatex, "-interaction=nonstopmode", "-output-directory", outdir, tex], cwd=outdir, env=env)
        if code != 0:
            break
        if aux_state(base) == before and "Rerun to get" not in log:
            break
    return code, log


def compare_one(job, args):
    n, label, tex_text, srcdir = job["n"], job["label"], job["tex"], job["srcdir"]
    d = os.path.join(args.out, "%04d" % n)
    mine, ref = os.path.join(d, "mine"), os.path.join(d, "ref")
    for p in (mine, ref):
        os.makedirs(p, exist_ok=True)
    for p in (mine, ref):
        with open(os.path.join(p, "doc.tex"), "w", encoding="utf-8") as fh:
            fh.write(tex_text)
    t0 = time.time()
    code_m, out_m = run([args.mep_tex, "compile", os.path.join(mine, "doc.tex"), "--dir", srcdir, "--packages", "bundle"])
    t_m = time.time() - t0
    t0 = time.time()
    code_r, out_r = compile_ref(args.ref_pdflatex, os.path.join(ref, "doc.tex"), ref, srcdir)
    t_r = time.time() - t0
    pm, pr = os.path.join(mine, "doc.pdf"), os.path.join(ref, "doc.pdf")
    have_m = os.path.exists(pm) and os.path.getsize(pm) > 0
    have_r = os.path.exists(pr) and os.path.getsize(pr) > 0
    res = {"n": n, "label": label, "t_mine": t_m, "t_ref": t_r, "dir": d}
    if not have_m and not have_r:
        res.update(status="both-failed", detail=first_error(out_m) + " | ref: " + first_error(out_r))
    elif not have_m:
        res.update(status="mine-failed", detail=first_error(out_m) or ("exit %d" % code_m))
    elif not have_r:
        res.update(status="ref-failed", detail=first_error(out_r) or ("exit %d" % code_r))
    else:
        na, nb, worst, per = raster_diff(pm, pr, d)
        ta, tb = pdf_text(pm), pdf_text(pr)
        text_same = ta == tb
        if na != nb:
            status = "differ"
            detail = "pages %d vs %d" % (na, nb)
        elif worst == 0 and text_same:
            status, detail = "same", ""
        elif worst < 0.01:
            status, detail = "close", "worst page %.3f%% pixels%s" % (100 * worst, "" if text_same else ", text differs")
        else:
            bad = [i + 1 for i, f in enumerate(per) if f >= 0.01]
            status = "differ"
            detail = "pages %s differ (worst %.1f%%)%s" % (",".join(map(str, bad[:8])), 100 * worst,
                                                         "" if text_same else ", text differs")
        if code_m != 0 and code_r == 0:
            detail = (detail + "; " if detail else "") + "mep-tex reported: " + first_error(out_m)
        res.update(status=status, detail=detail)
    if res["status"] == "same" and not args.keep:
        shutil.rmtree(d, ignore_errors=True)
    return res


def make_jobs(files, args):
    jobs = []
    work = os.path.join(args.out, "_src")
    os.makedirs(work, exist_ok=True)
    seen_frag = set()
    for f in files:
        srcdir = os.path.dirname(f)
        rel = os.path.relpath(f, REPO) if f.startswith(REPO) else f
        if f.endswith(".tex"):
            jobs.append({"label": rel, "tex": open(f, encoding="utf-8", errors="replace").read(), "srcdir": srcdir})
            continue
        variants = [("", [])]
        if f.endswith(".mepml") and re.search(r"^\s*\\slide\b", open(f, errors="replace").read(), re.M):
            variants.append((" [beamer]", ["--beamer"]))
        for suffix, flags in variants:
            out = os.path.join(work, hashlib.md5((f + suffix).encode()).hexdigest()[:12] + ".tex")
            code, log = run([args.mep_mepml, "convert"] + flags + [f, out], cwd=srcdir)
            if code == 0 and os.path.exists(out):
                jobs.append({"label": rel + suffix, "tex": open(out, encoding="utf-8").read(), "srcdir": srcdir})
            else:
                print("tex_compare: cannot export %s: %s" % (rel, log.strip()[:200]), file=sys.stderr)
        if args.fragments:
            for frag in fragments(f):
                key = frag.strip()
                if key in seen_frag:
                    continue
                seen_frag.add(key)
                jobs.append({"label": "%s: %s" % (rel, key[:60].replace("\n", " ")),
                             "tex": PREVIEW_PREAMBLE + frag + "\n\\end{document}\n", "srcdir": srcdir})
    for i, j in enumerate(jobs):
        j["n"] = i
    return jobs


ORDER = {"mine-failed": 0, "differ": 1, "close": 2, "ref-failed": 3, "both-failed": 4, "same": 5}


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("sources", nargs="+")
    ap.add_argument("--ref-pdflatex", required=True)
    ap.add_argument("--mep-tex", default=os.path.join(REPO, "build/native/mep-tex"))
    ap.add_argument("--mep-mepml", default=os.path.join(REPO, "build/native/mep-mepml"))
    ap.add_argument("--out", default="/tmp/tex-compare")
    ap.add_argument("--jobs", type=int, default=os.cpu_count() or 4)
    ap.add_argument("--fragments", action="store_true", help="also typeset each maths fragment as a preview")
    ap.add_argument("--keep", action="store_true", help="keep the files of documents that came out the same")
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)

    files = collect(args.sources)
    jobs = make_jobs(files, args)
    print("tex_compare: %d documents from %d files" % (len(jobs), len(files)), file=sys.stderr)
    # One compile first, so the bundle is unpacked and the formats are
    # made once rather than by every worker at the same time.
    results = []
    if jobs:
        warm = [j for j in jobs if "\\documentclass[11pt]{article}" in j["tex"]][:1] + jobs[:1]
        for j in warm[:1]:
            results.append(compare_one(j, args))
    rest = [j for j in jobs if not results or j["n"] != results[0]["n"]]
    with cf.ThreadPoolExecutor(max_workers=args.jobs) as ex:
        for r in ex.map(lambda j: compare_one(j, args), rest):
            results.append(r)
            if r["status"] not in ("same",):
                print("  %-11s %s  %s" % (r["status"], r["label"][:70], r.get("detail", "")[:100]), file=sys.stderr)

    results.sort(key=lambda r: (ORDER[r["status"]], r["label"]))
    counts = {}
    for r in results:
        counts[r["status"]] = counts.get(r["status"], 0) + 1
    t_m = sum(r["t_mine"] for r in results)
    t_r = sum(r["t_ref"] for r in results)
    lines = ["# mep-tex vs pdflatex", "",
             "%d documents: %s." % (len(results), ", ".join("%d %s" % (counts[k], k) for k in sorted(counts, key=ORDER.get))),
             "Compile time: mep-tex %.1f s, pdflatex %.1f s (summed over documents)." % (t_m, t_r), "",
             "| status | document | detail | files |", "|---|---|---|---|"]
    for r in results:
        if r["status"] == "same":
            continue
        lines.append("| %s | %s | %s | %s |" % (r["status"], r["label"].replace("|", "\\|"),
                                               r.get("detail", "").replace("|", "\\|"), r["dir"]))
    report = os.path.join(args.out, "report.md")
    with open(report, "w") as fh:
        fh.write("\n".join(lines) + "\n")
    print("\n".join(lines[:4]), file=sys.stderr)
    print("report: " + report, file=sys.stderr)
    return 1 if counts.get("mine-failed") else 0


if __name__ == "__main__":
    sys.exit(main())
