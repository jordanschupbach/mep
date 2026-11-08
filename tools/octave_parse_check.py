#!/usr/bin/env python3
"""Check mep-octave-lsp's reader against Octave's own: `__parse_file__`.

    just octave-parse-check                        # the whole corpus
    just octave-parse-check --sample 2000 --mutants 3
    just octave-parse-check --files a.m --show 20

For every file both say "ok" or an error, and the error must be the same:
Octave's message and its "near line L, column C". `--mutants N` adds N
damaged copies of each file (a slice deleted or duplicated, or a keyword /
bracket / quote inserted) to exercise the error paths.

The oracle is tools/octave_parse_oracle.m run by `octave-cli` (one process
per batch). Errors Octave raises *after* parsing -- a classdef's superclass
not on the path, a duplicate property -- carry no "near line" and are not
parse errors; they count as "ok".

The corpus is build/grammar-corpus/matlab/files.txt
(`python3 tools/ts_grammar_diff/grammar_diff.py corpus matlab`).
"""

import argparse
import collections
import concurrent.futures
import os
import random
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
BINARY = Path(os.environ.get('MEP_OCTAVE_LSP', REPO / 'build' / 'native' / 'mep-octave-lsp'))
ORACLE = REPO / 'tools' / 'octave_parse_oracle.m'
CORPUS = REPO / 'build' / 'grammar-corpus' / 'matlab' / 'files.txt'
KEEP = REPO / 'build' / 'octave-parse-check'  # damaged copies that disagreed, kept to look at
ENV = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}

INSERTS = ['end', 'end\n', 'endif', 'endfor', 'endfunction', 'end_try_catch', 'if ', 'else\n', 'elseif ',
           'for i = 1:2\n', 'while ', 'do\n', 'until ', 'switch ', 'case ', 'otherwise\n', 'try\n', 'catch\n',
           'unwind_protect\n', 'unwind_protect_cleanup\n', 'function ', 'function x = f (a)\n', 'global ',
           'break', 'continue', 'return', '(', ')', '[', ']', '{', '}', "'", '"', '%', '#', '%{\n', '%}\n',
           '...', '...\n', ',', ';', '\n', ' ', '=', '==', '~', '!', '~=', '!=', '@', '@(x) ', '.', '.(', ':',
           '+', '-', '*', '.*', "'", ".'", '^', '++', '+=', '&&', '||', '&', '|', '\\', 'x', '1', '1e', '0x',
           'end_unwind_protect', 'endwhile', 'endswitch', '__LINE__', 'x -1', 'hold on', '\\n', '\\x', '\\777']


def norm(line):
    """Octave's message without the file name; post-parse errors are 'ok'."""
    line = re.sub(r' in file .*$', '', line.strip())
    if line != 'ok' and ' near line ' not in line:
        return 'ok'
    # A classdef property default Octave evaluates while loading the class.
    if re.match(r"'[^']*' undefined near line", line):
        return 'ok'
    return line


def octave_verdicts(paths):
    with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
        f.write('\n'.join(str(p) for p in paths) + '\n')
        listing = f.name
    try:
        p = subprocess.run(['octave-cli', '--norc', '--quiet', '--no-gui', str(ORACLE), listing],
                           capture_output=True, timeout=1200, env=ENV)
    finally:
        os.unlink(listing)
    out = p.stdout.decode('utf-8', 'replace').split('\n')
    if len(out) < len(paths):
        return None
    return [norm(line) for line in out[:len(paths)]]


def mep_verdicts(paths):
    p = subprocess.run([str(BINARY), '--octave-check', *map(str, paths)], capture_output=True, timeout=600,
                       env=ENV)
    if p.returncode != 0:
        return None
    out = p.stdout.decode('utf-8', 'replace').split('\n')
    return [line.strip() for line in out[:len(paths)]]


def classify(oct_v, mep_v):
    if oct_v == mep_v:
        return 'agree'
    if oct_v == 'ok' or mep_v == 'ok':
        return 'octave-ok' if oct_v == 'ok' else 'mep-ok'
    om = re.match(r'(.*) near line (\d+), column (\d+)$', oct_v)
    mm = re.match(r'(.*) near line (\d+), column (\d+)$', mep_v)
    if om and mm:
        if om.group(1) == mm.group(1) and om.group(2) == mm.group(2):
            return 'column'
        if om.group(1) == mm.group(1):
            return 'line'
        if om.group(2) == mm.group(2):
            return 'message'
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
    ap.add_argument('--no-originals', action='store_true', help='with --mutants: only the damaged copies')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=10, help='examples to print per kind of disagreement')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    if not BINARY.exists():
        sys.exit(f'{BINARY} not built: cmake --build build/native --target mep-octave-lsp')

    rng = random.Random(args.seed)
    if args.files:
        files = args.files
    else:
        if not CORPUS.exists():
            sys.exit(f'no corpus at {CORPUS}: python3 tools/ts_grammar_diff/grammar_diff.py corpus matlab')
        files = [l for l in CORPUS.read_text().splitlines() if l]
        if args.sample and args.sample < len(files):
            files = rng.sample(files, args.sample)

    tmp = tempfile.TemporaryDirectory(prefix='octcheck-')
    work = []
    for i, f in enumerate(files):
        if not (args.mutants and args.no_originals):
            work.append((f, f))
        if args.mutants:
            try:
                data = Path(f).read_bytes()
            except OSError:
                continue
            # Keep the file name (a classdef must match it) and an @/+ folder.
            parent = Path(f).parent.name
            for j in range(args.mutants):
                d = Path(tmp.name) / f'm{i}_{j}'
                if parent.startswith(('@', '+')):
                    d = d / parent
                d.mkdir(parents=True)
                m = d / Path(f).name
                m.write_bytes(mutate(data, rng))
                work.append((str(m), f'{f} (mutant {j})'))

    batches = [work[k:k + 300] for k in range(0, len(work), 300)]
    tally = collections.Counter()
    examples = collections.defaultdict(list)
    crashed = []

    def run_batch(batch):
        paths = [p for p, _ in batch]
        res = mep_verdicts(paths)
        if res is None:
            out = []
            for p, label in batch:
                r = mep_verdicts([p])
                if r is None:
                    crashed.append(label)
                    out.append('CRASH')
                else:
                    out.append(r[0])
            res = out
        ref = octave_verdicts(paths)
        if ref is None:
            ref = []
            for p in paths:
                r = octave_verdicts([p])
                ref.append(r[0] if r else 'OCTAVE-FAILED')
        return [(label, path, o, m) for (path, label), o, m in zip(batch, ref, res)]

    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for results in ex.map(run_batch, batches):
            for label, path, o, m in results:
                kind = classify(o, m)
                tally[kind] += 1
                if kind != 'agree':
                    examples[kind].append((label, path, o, m))

    total = sum(tally.values())
    print(f'{total} files: {tally["agree"]} agree')
    for kind in ('octave-ok', 'mep-ok', 'column', 'line', 'message', 'line+message'):
        if tally[kind]:
            print(f'  {kind}: {tally[kind]}')
    if crashed:
        print(f'  crashed: {len(crashed)}')
        for c in crashed[:args.show]:
            print(f'    {c}')
    if any(examples.values()):
        shutil.rmtree(KEEP, ignore_errors=True)
        KEEP.mkdir(parents=True, exist_ok=True)
    for kind, items in examples.items():
        print(f'\n== {kind}')
        for n, (label, path, o, m) in enumerate(items[:args.show]):
            kept = path
            if '(mutant' in label:
                kept = KEEP / f'{kind}-{n}' / Path(path).name
                kept.parent.mkdir(parents=True, exist_ok=True)
                kept.write_bytes(Path(path).read_bytes())
            print(f'  {label}\n    octave: {o}\n    mep:    {m}\n    file:   {kept}')
    sys.exit(0 if tally['agree'] == total and not crashed else 1)


if __name__ == '__main__':
    main()
