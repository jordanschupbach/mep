#!/usr/bin/env python3
"""Check mep-scheme-lsp's reader against Guile's own `read`.

    just scheme-read-check                          # the whole corpus
    just scheme-read-check --mutants 1              # plus one damaged copy of each file
    just scheme-read-check --files a.scm --show 20

For every file both say "ok" or an error, and the error must be the same:
Guile's message (`unexpected ")"`, `unknown character name foo`, ...) at the
same LINE:COLUMN (Guile's column: characters, a tab to the next multiple of
8). Guile's reader runs with Guix's G-expression syntax installed
(tools/ts_grammar_diff/scheme_read.scm), as mep's reader reads it.
`--mutants N` adds N damaged copies of each file (a slice deleted or
duplicated, or a bracket, quote, `#` sequence or escape inserted) to
exercise the error paths.

The corpus is build/grammar-corpus/scheme/files.txt
(`just grammar-corpus scheme`).
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
BINARY = REPO / 'build' / 'native' / 'mep-scheme-lsp'
ORACLE = REPO / 'tools' / 'ts_grammar_diff' / 'scheme_read.scm'
CORPUS = REPO / 'build' / 'grammar-corpus' / 'scheme' / 'files.txt'
KEEP = REPO / 'build' / 'scheme-read-check'  # damaged copies that disagreed, kept to look at
ENV = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}

INSERTS = ['(', ')', '[', ']', '{', '}', '"', '\\', '#', '#\\', '#\\x', '#\\foo ', '#(', '#u8(', '#vu8(', '#v',
           '#|', '|#', '#;', '#!', '!#', '#!r6rs\n', '#!fold-case ', "'", '`', ',', ',@', "#'", '#`', '#,', '#,@',
           '#:', '#{', '}#', '#t', '#true', '#f', '#nil', '#n', '#*', '#x', '#e1.5', '#b2', '#z', '#2(', '#@',
           '#s8(', '#f64(', '.', ' . ', '|', ';', '\n', '\\x41;', '\\q', '\\u00', '#~', '#$', '#$@', '1e400']


def batch(argv, paths):
    """Runs a batch checker over paths; returns {path: verdict}."""
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
        f.write(''.join(str(p) + '\n' for p in paths))
        name = f.name
    try:
        p = subprocess.run(argv + [name], capture_output=True, timeout=3600, env=ENV)
    finally:
        os.unlink(name)
    out = {}
    for line in p.stdout.decode('utf-8', 'replace').split('\n'):
        if '\t' in line:
            path, verdict = line.split('\t', 1)
            out[path] = verdict
    return out


def guile(paths, jobs):
    chunks = [paths[i::jobs] for i in range(jobs)]
    out = {}
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for got in ex.map(lambda c: batch(['guile', '--no-auto-compile', '-s', str(ORACLE), '--batch'], c), chunks):
            out.update(got)
    return out


def mep(binary, paths):
    return batch([str(binary), '--guile-check-list'], paths)


def mutate(data, rng):
    if not data:
        return rng.choice(INSERTS).encode()
    at = rng.randrange(len(data) + 1)
    r = rng.random()
    if r < 0.3:
        n = rng.randint(1, 20)
        return data[:at] + data[at + n:]
    if r < 0.4:
        n = rng.randint(1, 40)
        return data[:at] + data[at:at + n] + data[at:]
    return data[:at] + rng.choice(INSERTS).encode() + data[at:]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--files', nargs='*', help='check these files instead of the corpus')
    ap.add_argument('--sample', type=int, help='a random sample of this many corpus files')
    ap.add_argument('--mutants', type=int, default=0, help='damaged copies per file')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=10, help='disagreements to print')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--binary', default=str(BINARY))
    a = ap.parse_args()

    rng = random.Random(a.seed)
    if a.files:
        files = [Path(f).resolve() for f in a.files]
    else:
        if not CORPUS.exists():
            sys.exit(f'no corpus at {CORPUS}; run: just grammar-corpus scheme')
        files = [Path(l) for l in CORPUS.read_text().splitlines() if l and os.path.exists(l)]
        if a.sample:
            files = rng.sample(files, min(a.sample, len(files)))
    paths = [str(f) for f in files]
    tmp = None
    if a.mutants:
        tmp = tempfile.TemporaryDirectory(prefix='scheme-read-check-')
        for i, f in enumerate(files):
            data = f.read_bytes()
            for m in range(a.mutants):
                p = Path(tmp.name) / f'{i}-{m}{f.suffix or ".scm"}'
                p.write_bytes(mutate(data, rng))
                paths.append(str(p))

    theirs = guile(paths, a.jobs)
    ours = mep(a.binary, paths)
    tally = collections.Counter()
    kinds = collections.Counter()
    shown = 0
    for p in paths:
        g, m = theirs.get(p), ours.get(p)
        if g is None or m is None:
            tally['no verdict'] += 1
            continue
        if g == m:
            tally['agree ok' if g == 'ok' else 'agree error'] += 1
            continue
        if g != 'ok' and m != 'ok' and not g[:1].isdigit() and not m[:1].isdigit():
            # An error Guile reports without a position comes from inside a
            # procedure the reader calls (list->typed-array, string->number);
            # its wording names that procedure's argument, so only the
            # verdict is compared.
            tally['agree error'] += 1
            tally['agree positionless'] += 1
            continue
        if (g == 'ok') != (m == 'ok'):
            kind = 'accept/reject differs'
        elif g.split(': ', 1)[-1] != m.split(': ', 1)[-1]:
            kind = 'message differs'
        elif g.split(':')[0] != m.split(':')[0]:
            kind = 'line differs'
        else:
            kind = 'column differs'
        tally[kind] += 1
        kinds[kind] += 1
        if shown < a.show:
            shown += 1
            print(f'{p}\n  guile: {g}\n  mep:   {m}')
            if tmp and p.startswith(tmp.name):
                KEEP.mkdir(parents=True, exist_ok=True)
                kept = KEEP / Path(p).name
                kept.write_bytes(Path(p).read_bytes())
                print(f'  kept as {kept}')
    total = sum(v for k, v in tally.items() if k != 'agree positionless')
    agree = tally['agree ok'] + tally['agree error']
    print(f'{total} files ({len(files)} originals, {total - len(files)} damaged): {agree} agree '
          f'({tally["agree ok"]} ok, {tally["agree error"]} same error, {tally["agree positionless"]} of them '
          f'an error Guile gives no position for)')
    for k in ('accept/reject differs', 'message differs', 'line differs', 'column differs', 'no verdict'):
        if tally[k]:
            print(f'  {k}: {tally[k]}')
    return 0 if agree == total else 1


if __name__ == '__main__':
    sys.exit(main())
