#!/usr/bin/env python3
"""Compares mep's Common Lisp formatter with Emacs on a corpus.

    tools/cl_format_compare.py FORMATTER FILES_TXT CACHE_DIR [--raw] [--jobs N] [--show N]

FORMATTER is `mep-lisp-format-test` (run as `FORMATTER --format FILE`),
FILES_TXT lists one source file per line, and CACHE_DIR keeps Emacs's
outputs (tools/cl_indent_oracle.el, run once per file and reused). Prints
how many files come out byte-identical, the share of identical lines, how
many files are idempotent, and the refusals; --show N prints the first N
differing hunks.
"""

import argparse
import difflib
import hashlib
import os
import subprocess
import sys
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
ORACLE = os.path.join(HERE, "cl_indent_oracle.el")


def env():
    e = dict(os.environ)
    e.pop("LD_LIBRARY_PATH", None)
    return e


def oracle(path, cache, raw):
    key = hashlib.sha1((("raw:" if raw else "") + path).encode()).hexdigest()
    with open(ORACLE, "rb") as f:
        key = hashlib.sha1(key.encode() + f.read() + open(path, "rb").read()).hexdigest()
    out = os.path.join(cache, key)
    if not os.path.exists(out):
        e = env()
        if raw:
            e["MEP_ORACLE_RAW"] = "1"
        tmp = out + ".tmp%d" % os.getpid()
        subprocess.run(["emacs", "-Q", "--batch", "-l", ORACLE, path, tmp], env=e, stdout=subprocess.DEVNULL,
                       stderr=subprocess.DEVNULL, timeout=600)
        if not os.path.exists(tmp):
            return None
        os.replace(tmp, out)
    with open(out, "rb") as f:
        return f.read()


def ours(formatter, data):
    p = subprocess.run([formatter, "--format", "/dev/stdin"], input=data, capture_output=True, env=env(), timeout=600)
    return p.returncode, p.stdout, p.stderr.decode(errors="replace").strip()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("formatter")
    ap.add_argument("files")
    ap.add_argument("cache")
    ap.add_argument("--raw", action="store_true")
    ap.add_argument("--jobs", type=int, default=os.cpu_count())
    ap.add_argument("--show", type=int, default=0)
    ap.add_argument("--list", help="write the differing files' paths here")
    a = ap.parse_args()
    os.makedirs(a.cache, exist_ok=True)
    files = [l.strip() for l in open(a.files) if l.strip()]

    def one(path):
        src = open(path, "rb").read()
        want = oracle(path, a.cache, a.raw)
        rc, got, err = ours(a.formatter, src)
        again = None
        if rc == 0:
            rc2, again, _ = ours(a.formatter, got)
        return path, want, rc, got, err, again

    with ThreadPoolExecutor(a.jobs) as ex:
        results = list(ex.map(one, files))

    same = refused = idem = lines_total = lines_same = no_oracle = 0
    shown = 0
    diffs = []
    for path, want, rc, got, err, again in results:
        if want is None:
            no_oracle += 1
            continue
        if rc != 0:
            refused += 1
            print("refused: %s: %s" % (path, err))
            continue
        if again == got:
            idem += 1
        else:
            print("not idempotent: %s" % path)
        wl = want.decode("utf-8", "surrogateescape").split("\n")
        gl = got.decode("utf-8", "surrogateescape").split("\n")
        lines_total += len(wl)
        sm = difflib.SequenceMatcher(None, wl, gl, autojunk=False)
        lines_same += sum(b.size for b in sm.get_matching_blocks())
        if want == got:
            same += 1
            continue
        diffs.append(path)
        if shown < a.show:
            shown += 1
            print("=== %s" % path)
            for l in list(difflib.unified_diff(wl, gl, "emacs", "mep", n=2, lineterm=""))[:40]:
                print(l)
    n = len(results) - no_oracle
    print("files: %d (oracle failed on %d)" % (len(results), no_oracle))
    print("identical: %d / %d (%.1f%%)" % (same, n, 100.0 * same / max(1, n)))
    print("lines identical: %d / %d (%.3f%%)" % (lines_same, lines_total, 100.0 * lines_same / max(1, lines_total)))
    print("idempotent: %d / %d formatted" % (idem, n - refused))
    print("refused: %d" % refused)
    if a.list:
        with open(a.list, "w") as f:
            f.write("".join(p + "\n" for p in diffs))


if __name__ == "__main__":
    sys.exit(main())
