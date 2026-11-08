#!/usr/bin/env python3
"""Check mep-tcl-lsp's parser (src/tcl_parse.cpp, a port of tclParse.c and
tclCompExpr.c) against Tcl's own, span by span.

    just tcl-parse-check                         # the whole corpus
    just tcl-parse-check --sample 500 --mutants 3
    just tcl-parse-check --files a.tcl --show 20

`mep-tcl-lsp --parse-report FILE` lists every script and expression the
analysis parsed (the file, proc bodies, if/while/switch/try bodies, [...]
substitutions, braced expressions...) with its verdict; tcl-parse-oracle
(tools/tcl_parse_oracle.c, linked against libtcl) parses the same spans with
Tcl_ParseCommand / Tcl_ParseExpr. They must agree on every span: ok or
error, the message, and for script errors the position Tcl points at
(parse.term), hence its line. `--mutants N` adds N damaged copies of each
file (a slice deleted or duplicated, or a bracket, brace, quote, `$`,
operator ... inserted) to exercise the error paths.

The corpus is build/grammar-corpus/tcl/files.txt (`just grammar-corpus tcl`).
"""

import argparse
import collections
import concurrent.futures
import os
import random
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BUILD = Path(os.environ.get('MEP_TCL_BUILD', REPO / 'build' / 'native'))
CORPUS = REPO / 'build' / 'grammar-corpus' / 'tcl' / 'files.txt'
KEEP = REPO / 'build' / 'tcl-parse-check'
ENV = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}

INSERTS = ['{', '}', '[', ']', '"', '$', '${', '$a(', '(', ')', '\\', '\\\n', ';', '\n', '#', '{*}', ' ',
           'x', '+', '-', '*', '**', '?', ':', ',', '==', '=', '!', '&&', '||', 'eq ', ' in ', '0x', '0o9', '1e',
           '08', 'sin(', 'foo(', '.5', 'expr {', 'if {', '} {', '} else {', 'proc p {} {', '"a"b', '{a}b']


def build_oracle():
    out = BUILD / 'tcl-parse-oracle'
    src = REPO / 'tools' / 'tcl_parse_oracle.c'
    if out.exists() and out.stat().st_mtime >= src.stat().st_mtime:
        return out
    tclsh = subprocess.run(['sh', '-c', 'readlink -f "$(command -v tclsh)"'], capture_output=True,
                           text=True).stdout.strip()
    if not tclsh:
        sys.exit('tclsh is not on PATH (run inside `nix develop`)')
    prefix = Path(tclsh).parent.parent
    subprocess.run(['cc', '-O1', str(src), f'-I{prefix}/include', f'-L{prefix}/lib', '-ltcl8.6',
                    f'-Wl,-rpath,{prefix}/lib', '-o', str(out)], check=True)
    return out


def check(binary, oracle, path):
    """[(kind, mep line, tcl line)] for the spans that disagree; None if mep crashed."""
    p = subprocess.run([str(binary), '--parse-report', path], capture_output=True, timeout=120, env=ENV)
    if p.returncode != 0:
        return None, 0, 0
    report = p.stdout
    q = subprocess.run([str(oracle), path], input=report, capture_output=True, timeout=120, env=ENV)
    mine = report.decode('utf-8', 'replace').splitlines()
    theirs = q.stdout.decode('utf-8', 'replace').splitlines()
    out = []
    for m, t in zip(mine, theirs):
        if m == t:
            continue
        mf, tf = m.split(' ', 4), t.split(' ', 4)
        if mf[0] == 'expr' and len(mf) == 6 or (mf[0] == 'expr' and len(mf) >= 5 and len(tf) >= 5):
            # Tcl's API has no position for an expression error: compare the messages.
            if mf[3] == tf[3] == 'error' and mf[4].split(' ', 1)[1:] == tf[4].split(' ', 1)[1:]:
                continue
        if mf[3:4] != tf[3:4]:
            kind = 'verdict'
        elif len(mf) > 4 and len(tf) > 4 and mf[4].split(' ', 1)[0] != tf[4].split(' ', 1)[0]:
            kind = 'position'
        else:
            kind = 'message'
        out.append((kind, m, t))
    return out, len(mine), sum(' error ' in l for l in mine)


def mutate(data, rng):
    if not data:
        return data
    k = rng.randrange(len(data))
    op = rng.randrange(3)
    if op == 0:
        return data[:k] + data[k + rng.randint(1, 12):]
    if op == 1:
        n = rng.randint(1, 12)
        return data[:k] + data[k:k + n] + data[k:]
    return data[:k] + rng.choice(INSERTS).encode() + data[k:]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--files', nargs='*', help='check these files instead of the corpus')
    ap.add_argument('--sample', type=int, default=0)
    ap.add_argument('--mutants', type=int, default=0, help='damaged copies per file')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=8)
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    binary = BUILD / 'mep-tcl-lsp'
    if not binary.exists():
        sys.exit(f'{binary} not built: cmake --build {BUILD} --target mep-tcl-lsp')
    oracle = build_oracle()
    rng = random.Random(args.seed)
    files = args.files or [l for l in CORPUS.read_text().splitlines() if l]
    if not args.files and args.sample and args.sample < len(files):
        files = rng.sample(files, args.sample)
    tmp = tempfile.TemporaryDirectory(prefix='tclcheck-')
    work = []
    for i, f in enumerate(files):
        work.append((f, f))
        if args.mutants:
            data = Path(f).read_bytes()
            for j in range(args.mutants):
                m = Path(tmp.name) / f'm{i}_{j}.tcl'
                m.write_bytes(mutate(data, rng))
                work.append((str(m), f'{f} (mutant {j})'))

    tally = collections.Counter()
    spans = collections.Counter()
    examples = collections.defaultdict(list)
    crashed = []

    def one(item):
        path, label = item
        try:
            return label, path, check(binary, oracle, path)
        except subprocess.TimeoutExpired:
            return label, path, ('timeout', 0, 0)

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for n, (label, path, (diffs, nspans, nerr)) in enumerate(ex.map(one, work), 1):
            if diffs is None:
                crashed.append(label)
            elif diffs == 'timeout':
                tally['timeout'] += 1
            else:
                spans['spans'] += nspans
                spans['errors'] += nerr
                spans['disagree'] += len(diffs)
                tally['agree' if not diffs else 'disagree'] += 1
                for kind, m, t in diffs:
                    spans[kind] += 1
                    if len(examples[kind]) < args.show:
                        keep = path
                        if path != label:
                            KEEP.mkdir(parents=True, exist_ok=True)
                            keep = str(KEEP / Path(path).name)
                            Path(keep).write_bytes(Path(path).read_bytes())
                        examples[kind].append((label, keep, m, t))
            if n % 200 == 0 or n == len(work):
                print(f'\r{n}/{len(work)}', end='', file=sys.stderr, flush=True)
    print(file=sys.stderr)
    print(f'{len(work)} files ({len(files)} originals, {args.mutants} mutants each); {len(crashed)} crashes')
    for k, v in tally.most_common():
        print(f'  files {k:12} {v:8}')
    print(f'  spans checked {spans["spans"]} ({spans["errors"]} of them errors), disagreeing {spans["disagree"]} '
          f'(verdict {spans["verdict"]}, position {spans["position"]}, message {spans["message"]})')
    for label in crashed[:args.show]:
        print(f'CRASH {label}')
    for kind, exs in examples.items():
        print(f'\n== {kind}')
        for label, path, m, t in exs:
            print(f'  {label}' + (f'  [{path}]' if path != label else ''))
            print(f'    mep: {m[:300]}')
            print(f'    tcl: {t[:300]}')
    return 1 if crashed or spans['disagree'] else 0


if __name__ == '__main__':
    sys.exit(main())
