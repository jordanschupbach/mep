#!/usr/bin/env python3
"""Check mep-lua-lsp's parser against Lua's own: `luac -p` (Lua 5.4).

    just lua-parse-check                       # the whole corpus
    just lua-parse-check --sample 3000 --mutants 3
    just lua-parse-check --files a.lua --show 20

For every file both say "ok" or an error, and the error must be the same
text: `FILE:LINE: message near 'token'` -- message, token and line.
`--mutants N` adds N damaged copies of each file (a slice deleted or
duplicated, or a keyword/bracket/quote inserted) to exercise the error paths.

The corpus is build/grammar-corpus/lua/files.txt
(`python3 tools/ts_grammar_diff/grammar_diff.py corpus lua`).
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
BINARY = REPO / 'build' / 'native' / 'mep-lua-lsp'
CORPUS = REPO / 'build' / 'grammar-corpus' / 'lua' / 'files.txt'
KEEP = REPO / 'build' / 'lua-parse-check'  # damaged copies that disagreed, kept to look at
ENV = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}

INSERTS = ['{', '}', '(', ')', '[', ']', ';', '"', "'", '[[', ']]', '[==[', '--[[', 'end ', 'local ', 'function ',
           'then ', 'do ', 'if ', 'for ', 'in ', 'return ', 'break ', 'goto x ', '::x::', '=', ',', '.', ':', '...',
           '..', '\\', '\\z', '\\x4', '\\u{', '0x', '1e', '<const>', '<close>', '\n', 'x', '#', '~', '//']


def luac_verdict(path):
    p = subprocess.run(['luac', '-p', str(path)], capture_output=True, timeout=60, env=ENV)
    if p.returncode == 0:
        return 'ok'
    err = p.stderr.decode('utf-8', 'replace').strip()
    if err.startswith('luac: '):
        err = err[len('luac: '):]
    return err.replace('\n', ' ')


def mep_verdicts(paths):
    p = subprocess.run([str(BINARY), '--luac-check', *map(str, paths)], capture_output=True, timeout=600, env=ENV)
    if p.returncode != 0:
        return None
    out = p.stdout.decode('utf-8', 'replace').split('\n')
    return out[:len(paths)]


def compare(path, mep_line):
    luac = luac_verdict(path)
    if luac == mep_line:
        return 'agree', luac, mep_line
    if luac == 'ok' or mep_line == 'ok':
        return ('luac-ok' if luac == 'ok' else 'mep-ok'), luac, mep_line
    lm = re.match(r'.*?:(\d+): (.*)$', luac)
    mm = re.match(r'.*?:(\d+): (.*)$', mep_line)
    if lm and mm and lm.group(2) == mm.group(2):
        return 'line', luac, mep_line
    return 'message', luac, mep_line


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
        sys.exit(f'{BINARY} not built: cmake --build build/native --target mep-lua-lsp')

    rng = random.Random(args.seed)
    if args.files:
        files = args.files
    else:
        if not CORPUS.exists():
            sys.exit(f'no corpus at {CORPUS}: python3 tools/ts_grammar_diff/grammar_diff.py corpus lua')
        files = [l for l in CORPUS.read_text().splitlines() if l]
        if args.sample and args.sample < len(files):
            files = rng.sample(files, args.sample)

    tmp = tempfile.TemporaryDirectory(prefix='luacheck-')
    work = []
    for i, f in enumerate(files):
        work.append((f, f))
        if args.mutants:
            try:
                data = Path(f).read_bytes()
            except OSError:
                continue
            for j in range(args.mutants):
                m = Path(tmp.name) / f'm{i}_{j}.lua'
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
                results.append((label, p, 'timeout', None, None))
        return results

    done = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for results in ex.map(run_batch, batches):
            for label, p, kind, nix, mep in results:
                tally[('agree: ok' if nix == 'ok' else 'agree: error') if kind == 'agree' else kind] += 1
                if kind not in ('agree', 'timeout') and len(examples[kind]) < args.show:
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
            print(f'    luac: {nix}')
            print(f'    mep: {mep}')
    return 1 if crashed or any(not k.startswith('agree') and k != 'timeout' for k in tally) else 0


if __name__ == '__main__':
    sys.exit(main())
