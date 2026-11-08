#!/usr/bin/env python3
"""Check mep's Go parser (src/go_parse.cpp) against go/parser itself.

    just go-parse-check                       # the whole corpus
    just go-parse-check --sample 3000 --mutants 3
    just go-parse-check --files a.go --show 20

The oracle is tools/ts_grammar_diff/go_check.go (`just go-check`), which
parses with go/parser in gofmt's mode (ParseComments|AllErrors|
SkipObjectResolution) and prints every error. mep's side is
`mep-go-lsp --parse-check FILE...`, one line per file: "ok" or the errors
as `LINE:COL: message` joined by " | " (a newline inside a message as `\\n`). For every file the two lists must be
identical -- same messages, same positions, same order. `--mutants N` adds N
damaged copies of each file (a slice deleted or duplicated, or a token
inserted) to exercise the error paths.

The corpus is build/grammar-corpus/go/files.txt
(`python3 tools/ts_grammar_diff/grammar_diff.py corpus go`).
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
BINARY = Path(os.environ.get('MEP_GO_LSP', REPO / 'build' / 'native' / 'mep-go-lsp'))
ORACLE = REPO / 'build' / 'grammar-diff' / 'go' / 'go-check'
CORPUS = REPO / 'build' / 'grammar-corpus' / 'go' / 'files.txt'
KEEP = REPO / 'build' / 'go-parse-check'  # damaged copies that disagreed, kept to look at
ENV = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}

INSERTS = ['{', '}', '(', ')', '[', ']', ';', '"', "'", '`', ',', '.', ':', ':=', '=', '...', '<-', '~', '|', '*',
           '&', '/*', '//', '\n', 'x', '0x', '1e', '0b2', '1_', '\\', '\\u', 'func ', 'type ', 'var ', 'const ',
           'import ', 'package ', 'if ', 'else ', 'for ', 'range ', 'switch ', 'case ', 'default:', 'select ',
           'go ', 'defer ', 'return ', 'break ', 'goto ', 'chan ', 'map[', 'struct{', 'interface{', '[T any]',
           '//line x.go:0\n', '“', '\x00', '\xef\xbb\xbf']


def oracle_verdict(path):
    p = subprocess.run([str(ORACLE), str(path)], capture_output=True, timeout=60, env=ENV)
    if p.returncode == 0:
        return 'ok'
    errs = []
    prefix = str(path) + ':'
    text = p.stderr.decode('utf-8', 'replace')
    if text.endswith('\n'):
        text = text[:-1]
    for line in text.split('\n'):
        if line.startswith(prefix):
            errs.append(line[len(prefix):])
        elif errs:
            errs[-1] += '\\n' + line  # a message with a newline in it (a token's text)
        else:
            errs.append(line)
    return ' | '.join(errs)


def mep_verdicts(paths):
    p = subprocess.run([str(BINARY), '--parse-check', *map(str, paths)], capture_output=True, timeout=600, env=ENV)
    if p.returncode != 0:
        return None
    out = p.stdout.decode('utf-8', 'replace').split('\n')
    return out[:len(paths)]


def compare(path, mep_line):
    go = oracle_verdict(path)
    if go == mep_line:
        return 'agree', go, mep_line
    if go == 'ok' or mep_line == 'ok':
        return ('go-ok' if go == 'ok' else 'mep-ok'), go, mep_line
    return 'differ', go, mep_line


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
    return data[:k] + rng.choice(INSERTS).encode('utf-8', 'surrogatepass') + data[k:]


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
        sys.exit(f'{BINARY} not built: cmake --build build/native --target mep-go-lsp')
    if not ORACLE.exists():
        sys.exit(f'{ORACLE} not built: just go-check')

    rng = random.Random(args.seed)
    if args.files:
        files = args.files
    else:
        if not CORPUS.exists():
            sys.exit(f'no corpus at {CORPUS}: python3 tools/ts_grammar_diff/grammar_diff.py corpus go')
        files = [l for l in CORPUS.read_text().splitlines() if l]
        if args.sample and args.sample < len(files):
            files = rng.sample(files, args.sample)

    KEEP.mkdir(parents=True, exist_ok=True)
    tmp = tempfile.TemporaryDirectory(prefix='gocheck-', dir=KEEP)
    work = []
    for i, f in enumerate(files):
        work.append((f, f))
        if args.mutants:
            try:
                data = Path(f).read_bytes()
            except OSError:
                continue
            for j in range(args.mutants):
                m = Path(tmp.name) / f'm{i}_{j}.go'
                m.write_bytes(mutate(data, rng))
                work.append((str(m), f'{f} (mutant {j})'))

    batches = [work[k:k + 200] for k in range(0, len(work), 200)]
    tally = collections.Counter()
    examples = collections.defaultdict(list)
    crashed = []

    def run_batch(batch):
        res = mep_verdicts([p for p, _ in batch])
        if res is None:
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
            for label, p, kind, go, mep in results:
                tally[('agree: ok' if go == 'ok' else 'agree: error') if kind == 'agree' else kind] += 1
                if kind not in ('agree', 'timeout') and len(examples[kind]) < args.show:
                    keep = p if p == label else str(KEEP / Path(p).name)
                    if keep != p:
                        Path(keep).write_bytes(Path(p).read_bytes())
                    examples[kind].append((label, keep, go, mep))
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
        for label, path, go, mep in exs:
            print(f'  {label}' + (f'  [{path}]' if path != label else ''))
            print(f'    go:  {go[:600]}')
            print(f'    mep: {mep[:600]}')
    return 1 if crashed or any(not k.startswith('agree') and k != 'timeout' for k in tally) else 0


if __name__ == '__main__':
    sys.exit(main())
