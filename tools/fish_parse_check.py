#!/usr/bin/env python3
"""Check mep-fish-lsp's reader against fish's own: `fish --no-execute FILE`.

    just fish-parse-check                       # the whole corpus
    just fish-parse-check --sample 3000 --mutants 3
    just fish-parse-check --files a.fish --show 20

For every file both say "ok" or an error, and the error must be the same:
`(line N): message` -- fish's first error, its line and its text.
`--mutants N` adds N damaged copies of each file (a slice deleted or
duplicated, or a keyword/bracket/quote inserted) to exercise the error paths.

The corpus is build/grammar-corpus/fish/files.txt
(`python3 tools/ts_grammar_diff/grammar_diff.py corpus fish`).
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
BINARY = Path(os.environ.get('MEP_FISH_LSP', REPO / 'build' / 'native' / 'mep-fish-lsp'))
CORPUS = REPO / 'build' / 'grammar-corpus' / 'fish' / 'files.txt'
KEEP = REPO / 'build' / 'fish-parse-check'  # damaged copies that disagreed, kept to look at
ENV = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}

INSERTS = ['{', '}', '(', ')', '[', ']', ';', '"', "'", '$', '$(', '${', '$?', '$#', '\\', '\\x', '\\u', '\\c',
           'end ', 'end\n', 'begin ', 'begin\n', 'function ', 'if ', 'else ', 'else if ', 'switch ', 'case ',
           'for ', 'in ', 'while ', 'not ', 'and ', 'or ', 'time ', 'exec ', 'builtin ', 'command ', 'break ',
           'continue ', 'return ', '|', '&', '&&', '||', '>', '<', '2>', '>&2', '0|', '>|', '&>', 'a=b ', '#',
           '\n', ' ', 'x', '[0]', '[a]', '..', '-h ', '--help ']


def strip_path(line, path):
    """`PATH (line N): msg` -> `(line N): msg`; `PATH: msg` -> `msg`."""
    for p in (str(path), str(path).replace(os.path.expanduser('~'), '~', 1)):
        if line.startswith(p + ' (line '):
            return line[len(p) + 1:]
        if line.startswith(p + ': '):
            return line[len(p) + 2:]
    return line


def fish_verdict(path):
    p = subprocess.run(['fish', '--no-execute', '--no-config', str(path)], capture_output=True, timeout=60,
                       env=ENV)
    err = p.stderr.decode('utf-8', 'replace')
    if p.returncode == 0 and not err.strip():
        return 'ok'
    first = err.split('\n', 1)[0]
    return strip_path(first, path)


def mep_verdicts(paths):
    p = subprocess.run([str(BINARY), '--fish-check', *map(str, paths)], capture_output=True, timeout=600, env=ENV)
    if p.returncode != 0:
        return None
    out = p.stdout.decode('utf-8', 'replace').split('\n')
    return [strip_path(line, path) for line, path in zip(out[:len(paths)], paths)]


def compare(path, mep_line):
    fishv = fish_verdict(path)
    if fishv == mep_line:
        return 'agree', fishv, mep_line
    if fishv == 'ok' or mep_line == 'ok':
        return ('fish-ok' if fishv == 'ok' else 'mep-ok'), fishv, mep_line
    lm = re.match(r'\(line (\d+)\): (.*)$', fishv)
    mm = re.match(r'\(line (\d+)\): (.*)$', mep_line)
    if lm and mm and lm.group(2) == mm.group(2):
        return 'line', fishv, mep_line
    if (lm.group(1) if lm else None) == (mm.group(1) if mm else None):
        return 'message', fishv, mep_line
    return 'line+message', fishv, mep_line


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
        sys.exit(f'{BINARY} not built: cmake --build build/native --target mep-fish-lsp')

    rng = random.Random(args.seed)
    if args.files:
        files = args.files
    else:
        if not CORPUS.exists():
            sys.exit(f'no corpus at {CORPUS}: python3 tools/ts_grammar_diff/grammar_diff.py corpus fish')
        files = [l for l in CORPUS.read_text().splitlines() if l]
        if args.sample and args.sample < len(files):
            files = rng.sample(files, args.sample)

    tmp = tempfile.TemporaryDirectory(prefix='fishcheck-')
    work = []
    for i, f in enumerate(files):
        work.append((f, f))
        if args.mutants:
            try:
                data = Path(f).read_bytes()
            except OSError:
                continue
            for j in range(args.mutants):
                m = Path(tmp.name) / f'm{i}_{j}.fish'
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
            print(f'    fish: {nix}')
            print(f'    mep: {mep}')
    return 1 if crashed or any(not k.startswith('agree') and k != 'timeout' for k in tally) else 0


if __name__ == '__main__':
    sys.exit(main())
