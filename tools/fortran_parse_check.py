#!/usr/bin/env python3
"""Check mep-fortran-lsp's reader against gfortran's: `gfortran -fsyntax-only`.

    just fortran-parse-check                       # the whole corpus
    just fortran-parse-check --sample 3000 --mutants 2
    just fortran-parse-check --files a.f90 --show 20

For every file both say "ok" or a syntax error, and ideally the same one:
`LINE: message`, gfortran's first syntax error (line 0 = end of file). gfortran
has no parse-only mode, so its verdict comes from
tools/ts_grammar_diff/fortran_check.py, which compiles against .mod files built
from the corpus, stubs modules and includes it cannot find and keeps only the
messages that are syntax (a type mismatch is not the reader's business).
`--mutants N` adds N damaged copies of each file (a slice deleted or
duplicated, or a keyword/bracket/quote inserted) to exercise the error paths.

gfortran is slow (a process per file); `--cache FILE` keeps its verdicts on the
originals between runs (mutants are always checked fresh).

The corpus is build/grammar-corpus/fortran/files.txt
(`python3 tools/ts_grammar_diff/grammar_diff.py corpus fortran`).
"""

import argparse
import collections
import concurrent.futures
import json
import os
import random
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools' / 'ts_grammar_diff'))
import fortran_check  # noqa: E402

BINARY = Path(os.environ.get('MEP_FORTRAN_LSP', REPO / 'build' / 'native' / 'mep-fortran-lsp'))
CORPUS = REPO / 'build' / 'grammar-corpus' / 'fortran' / 'files.txt'
KEEP = REPO / 'build' / 'fortran-parse-check'  # damaged copies that disagreed, kept to look at
ENV = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}

INSERTS = ['(', ')', '[', ']', '(/', '/)', ',', '=', '=>', '::', '%', "'", '"', '&', '&\n', ';', '!', '\n',
           'end ', 'end\n', 'end do\n', 'end if\n', 'endif\n', 'do ', 'do i = 1, n\n', 'if (x) then\n',
           'else\n', 'else if (y) then\n', 'select case (k)\n', 'case (1)\n', 'call ', 'call f(',
           'subroutine s\n', 'function f()\n', 'contains\n', 'module m\n', 'program p\n', 'integer :: ',
           'real, ', 'type(t) :: ', 'type t\n', 'end type\n', 'interface\n', 'use m\n', 'implicit none\n',
           'goto 10\n', '10 continue\n', 'x = ', '.and.', '.eq.', '**', '//', 'print *, ', 'write(*,*) ',
           'allocate(', 'where (m)\n', 'associate (a => b)\n', 'block\n', '#if 0\n', '#endif\n', 'x']


def gfortran_verdict(path):
    """'ok', `LINE: message`, or None (timeout / no verdict)."""
    r = fortran_check.check(path)
    if r['verdict'] == 'timeout':
        return None
    syntax = [e for e in r['errors'] if e['cls'] == 'syntax']
    if not syntax:
        return 'ok'
    e = syntax[0]
    return f"{e['line']}: {e['msg']}"


def mep_verdicts(paths):
    p = subprocess.run([str(BINARY), '--gfortran-check', *map(str, paths)], capture_output=True, timeout=600,
                       env=ENV)
    if p.returncode != 0:
        return None
    return p.stdout.decode('utf-8', 'replace').split('\n')[:len(paths)]


def compare(g, m):
    if g == m:
        return 'agree'
    if g == 'ok' or m == 'ok':
        return 'gfortran-ok' if g == 'ok' else 'mep-ok'
    gl, _, gm = g.partition(': ')
    ml, _, mm = m.partition(': ')
    if gl == ml:
        return 'message'
    if gm == mm:
        return 'line'
    return 'line+message'


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
    ap.add_argument('--sample', type=int, default=0, help='check a random sample of N corpus files')
    ap.add_argument('--mutants', type=int, default=0, help='damaged copies per file')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=10, help='examples to print per kind of disagreement')
    ap.add_argument('--cache', help='JSON lines of gfortran verdicts on the originals (read and extended)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    if not BINARY.exists():
        sys.exit(f'{BINARY} not built: cmake --build build/native --target mep-fortran-lsp')

    rng = random.Random(args.seed)
    if args.files:
        files = args.files
    else:
        if not CORPUS.exists():
            sys.exit(f'no corpus at {CORPUS}: python3 tools/ts_grammar_diff/grammar_diff.py corpus fortran')
        files = [l for l in CORPUS.read_text().splitlines() if l]
        if args.sample and args.sample < len(files):
            files = rng.sample(files, args.sample)

    cache = {}
    if args.cache and Path(args.cache).exists():
        for line in Path(args.cache).read_text().splitlines():
            r = json.loads(line)
            if 'verdict' in r and 'errors' in r:  # an oracle_all-style record
                syntax = [e for e in r['errors'] if e['cls'] == 'syntax']
                cache[r['file']] = 'ok' if not syntax else f"{syntax[0]['line']}: {syntax[0]['msg']}"
            else:
                cache[r['file']] = r['gfortran']
    new_cache = {}

    tmp = tempfile.TemporaryDirectory(prefix='fortrancheck-')
    work = []
    for i, f in enumerate(files):
        work.append((f, f, False))
        if args.mutants:
            try:
                data = Path(f).read_bytes()
            except OSError:
                continue
            for j in range(args.mutants):
                m = Path(tmp.name) / f'm{i}_{j}{Path(f).suffix}'
                m.write_bytes(mutate(data, rng))
                work.append((str(m), f'{f} (mutant {j})', True))

    batches = [work[k:k + 100] for k in range(0, len(work), 100)]
    tally = collections.Counter()
    examples = collections.defaultdict(list)
    crashed = []

    def run_batch(batch):
        res = mep_verdicts([p for p, _, _ in batch])
        if res is None:
            res = []
            for p, label, _ in batch:
                r = mep_verdicts([p])
                if r is None:
                    crashed.append(label)
                res.append(None if r is None else r[0])
        results = []
        for (p, label, mutant), m in zip(batch, res):
            if m is None:
                continue
            g = None if mutant else cache.get(p)
            if g is None:
                g = gfortran_verdict(p)
                if g is not None and not mutant:
                    new_cache[p] = g
            if g is None:
                results.append((label, p, 'timeout', None, m))
                continue
            results.append((label, p, compare(g, m), g, m))
        return results

    done = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for results in ex.map(run_batch, batches):
            for label, p, kind, g, m in results:
                tally[('agree: ok' if g == 'ok' else 'agree: error') if kind == 'agree' else kind] += 1
                if kind not in ('agree', 'timeout') and len(examples[kind]) < args.show:
                    KEEP.mkdir(parents=True, exist_ok=True)
                    keep = p if p == label else str(KEEP / Path(p).name)
                    if keep != p:
                        shutil.copyfile(p, keep)
                    examples[kind].append((label, keep, g, m))
            done += len(results)
            print(f'\r{done}/{len(work)}', end='', file=sys.stderr, flush=True)
    print(file=sys.stderr)
    if args.cache and new_cache:
        with open(args.cache, 'a') as out:
            for f, g in new_cache.items():
                out.write(json.dumps({'file': f, 'gfortran': g}) + '\n')

    total = sum(tally.values())
    print(f'{total} files ({len(files)} originals, {args.mutants} mutants each); {len(crashed)} crashes')
    for kind, n in tally.most_common():
        print(f'  {kind:24} {n:7}  {100.0 * n / max(total, 1):6.2f}%')
    verdict_agree = tally['agree: ok'] + tally['agree: error'] + tally['message'] + tally['line'] + tally['line+message']
    print(f'  verdicts agree (ok/error) {verdict_agree} of {total} ({100.0 * verdict_agree / max(total, 1):.2f}%)')
    for label in crashed[:args.show]:
        print(f'CRASH {label}')
    for kind, exs in examples.items():
        print(f'\n== {kind}')
        for label, path, g, m in exs:
            print(f'  {label}' + (f'  [{path}]' if path != label else ''))
            print(f'    gfortran: {g}')
            print(f'    mep:      {m}')
    return 1 if crashed else 0


if __name__ == '__main__':
    sys.exit(main())
