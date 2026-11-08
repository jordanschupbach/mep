#!/usr/bin/env python3
"""Check mep-nix-lsp's parser against Nix's own: `nix-instantiate --parse`.

    just nix-parse-check                       # the whole corpus
    just nix-parse-check --sample 3000 --mutants 3
    just nix-parse-check --files a.nix b.nix --show 20

For every file, both say one of:
  ok                       it parses and every variable is bound;
  a parse error            message and line:col must be identical;
  an undefined variable    Nix names the first one it meets (its order
                           depends on its symbol table), so that name and
                           position must be among the ones mep reports.

`--mutants N` adds N damaged copies of each file (a token-ish slice
deleted, duplicated, or a bracket/quote/keyword inserted), so the error
paths and their positions get exercised, not only valid code.

The corpus is build/grammar-corpus/nix/files.txt
(`python3 tools/ts_grammar_diff/grammar_diff.py corpus nix`).
"""

import argparse
import collections
import concurrent.futures
import os
import random
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BINARY = REPO / 'build' / 'native' / 'mep-nix-lsp'
CORPUS = REPO / 'build' / 'grammar-corpus' / 'nix' / 'files.txt'
KEEP = REPO / 'build' / 'nix-parse-check'  # damaged copies that disagreed, kept to look at
ENV = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}

INSERTS = ['{', '}', '(', ')', '[', ']', ';', '"', "''", '${', 'let ', ' in ', 'rec ', 'with ', 'inherit ',
           '=', ':', '@', '?', '.', ' or ', '...', './', '<', '>', '#', '/*', '\n', '-', '//', '|>', 'x', '1.',
           'http://a.b', '~/', '\\', '$', ',']


def line_col(data, off):
    """Nix's line and column: 1-based, bytes, \\r\\n, \\r and \\n end a line."""
    line, col, i = 1, 1, 0
    while i < off and i < len(data):
        c = data[i]
        if c == 0x0D:
            if i + 1 < len(data) and data[i + 1] == 0x0A and i + 1 < off:
                i += 1
            line, col = line + 1, 1
        elif c == 0x0A:
            line, col = line + 1, 1
        else:
            col += 1
        i += 1
    return line, col


def nix_verdict(path):
    p = subprocess.run(['nix-instantiate', '--parse', str(path)], capture_output=True, timeout=60, env=ENV)
    if p.returncode == 0:
        return ('ok',)
    err = p.stderr.decode('utf-8', 'replace').replace(str(Path(path).resolve()), '«file»').replace(str(path), '«file»')
    lines = err.splitlines()
    first = next((l for l in lines if l.startswith('error:')), lines[0] if lines else '')
    msg = first[len('error: '):] if first.startswith('error: ') else first
    pos = None
    for l in lines:
        m = re.match(r'\s+at «file»:(\d+):(\d+):$', l)
        if m:
            pos = (int(m.group(1)), int(m.group(2)))
    m = re.match(r"undefined variable '(.*)'$", msg)
    if m:
        return ('undefined', pos, m.group(1))
    return ('parse', pos, msg)


def mep_verdicts(paths):
    p = subprocess.run([str(BINARY), '--nix-check', *map(str, paths)], capture_output=True, timeout=600, env=ENV)
    if p.returncode != 0:
        return None
    out = p.stdout.decode('utf-8', 'replace').replace('\r', '').split('\n')
    return out[:len(paths)]


def compare(path, mep_line):
    data = Path(path).read_bytes()
    nix = nix_verdict(path)
    f = mep_line.split('\t')
    if f[0] == 'parse':
        mep = ('parse', line_col(data, int(f[1])), f[2].replace(str(path), '«file»'))
    elif f[0] == 'undefined':
        names = [(line_col(data, int(f[k])), f[k + 1]) for k in range(1, len(f) - 1, 2)]
        mep = ('undefined', names)
    else:
        mep = (f[0],)
    if nix[0] == 'ok' and mep[0] == 'ok':
        return 'agree', nix, mep
    if nix[0] == 'parse' and mep[0] == 'parse':
        if nix[2] != mep[2]:
            return 'message', nix, mep
        if nix[1] != mep[1]:
            return 'position', nix, mep
        return 'agree', nix, mep
    if nix[0] == 'undefined' and mep[0] == 'undefined':
        return ('agree' if (nix[1], nix[2]) in mep[1] else 'undefined-mismatch'), nix, mep
    return f'{nix[0]}-vs-{mep[0]}', nix, mep


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
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    if not BINARY.exists():
        sys.exit(f'{BINARY} not built: cmake --build build/native --target mep-nix-lsp')

    rng = random.Random(args.seed)
    if args.files:
        files = args.files
    else:
        if not CORPUS.exists():
            sys.exit(f'no corpus at {CORPUS}: python3 tools/ts_grammar_diff/grammar_diff.py corpus nix')
        files = [l for l in CORPUS.read_text().splitlines() if l]
        if args.sample and args.sample < len(files):
            files = rng.sample(files, args.sample)

    tmp = tempfile.TemporaryDirectory(prefix='nixcheck-')
    work = []
    for i, f in enumerate(files):
        work.append((f, f))
        if args.mutants:
            try:
                data = Path(f).read_bytes()
            except OSError:
                continue
            for j in range(args.mutants):
                m = Path(tmp.name) / f'm{i}_{j}.nix'
                m.write_bytes(mutate(data, rng))
                work.append((str(m), f'{f} (mutant {j})'))

    batches = [work[k:k + 200] for k in range(0, len(work), 200)]
    tally = collections.Counter()
    examples = collections.defaultdict(list)
    crashed = []

    def run_batch(batch):
        res = mep_verdicts([p for p, _ in batch])
        if res is None:
            # Find the file that crashed.
            out = []
            for p, label in batch:
                r = mep_verdicts([p])
                if r is None:
                    crashed.append(label)
                    out.append(None)
                else:
                    out.append(r[0])
            res = out
        results = []
        for (p, label), line in zip(batch, res):
            if line is None:
                continue
            try:
                results.append((label, p) + compare(p, line))
            except subprocess.TimeoutExpired:
                results.append((label, p, 'nix-timeout', None, None))
        return results

    done = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for results in ex.map(run_batch, batches):
            for label, p, kind, nix, mep in results:
                tally[f'agree: {nix[0]}' if kind == 'agree' else kind] += 1
                if kind not in ('agree', 'nix-timeout') and len(examples[kind]) < args.show:
                    KEEP.mkdir(parents=True, exist_ok=True)
                    keep = p if p == label else str(KEEP / Path(p).name)
                    if keep != p:
                        Path(keep).write_bytes(Path(p).read_bytes())
                    examples[kind].append((label, keep, nix, mep))
            done += len(results)
            print(f'\r{done}/{len(work)}', end='', file=sys.stderr, flush=True)
    print(file=sys.stderr)

    total = sum(tally.values())
    print(f'{total} files ({len(files)} originals, {args.mutants} mutants each); {len(crashed)} crashes')
    for kind, n in tally.most_common():
        print(f'  {kind:24} {n:7}  {100.0 * n / max(total, 1):6.2f}%')
    for label in crashed[:args.show]:
        print(f'CRASH {label}')
    for kind, exs in examples.items():
        print(f'\n== {kind}')
        for label, path, nix, mep in exs:
            print(f'  {label}' + (f'  [{path}]' if path != label else ''))
            print(f'    nix: {nix}')
            print(f'    mep: {mep}')
    return 1 if crashed or any(not k.startswith('agree') and k != 'nix-timeout' for k in tally) else 0


if __name__ == '__main__':
    sys.exit(main())
