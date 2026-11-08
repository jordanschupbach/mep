#!/usr/bin/env python3
"""Check mep-gap-lsp's reader against GAP's own: `ReadAsFunction(FILE)`.

    just gap-parse-check                         # the whole corpus
    just gap-parse-check --sample 2000 --mutants 3
    just gap-parse-check --files a.g --show 20

For every file both say "ok" or a syntax error, and the error must be the
same: GAP's first error, its message and its line (`LINE: message`); the
column is compared too where GAP printed a caret, and reported separately.
GAP reads the file as a function body (tools/ts_grammar_diff/gap_check.py,
one GAP for many files), so mep-gap-lsp is asked the same question
(`--gap-check`, which parses in that mode). Files GAP gave up on with a
run-time error while reading (an eager float literal) are skipped.

`--mutants N` adds N damaged copies of each file (a slice deleted or
duplicated, or a keyword/bracket/quote inserted) to exercise the error paths.

The corpus is build/grammar-corpus/gap/files.txt
(`python3 tools/ts_grammar_diff/grammar_diff.py corpus gap`).
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
BINARY = Path(os.environ.get('MEP_GAP_LSP', REPO / 'build' / 'native' / 'mep-gap-lsp'))
CORPUS = REPO / 'build' / 'grammar-corpus' / 'gap' / 'files.txt'
KEEP = REPO / 'build' / 'gap-parse-check'  # damaged copies that disagreed, kept to look at
ENV = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}
sys.path.insert(0, str(REPO / 'tools' / 'ts_grammar_diff'))
import gap_check  # noqa: E402

INSERTS = ['(', ')', '[', ']', '{', '}', ';', ';;', ',', ':=', ':', '"', '"""', "'", '\\', '\\0', '#', '.', '..',
           '...', '!.', '![', '->', '~', '^', '-', '+', '*', '/', '=', '<>', 'not ', ' and ', ' or ', ' mod ',
           ' in ', 'if ', ' then ', 'elif ', 'else ', ' fi', 'for ', 'while ', ' do ', ' od', 'repeat ',
           ' until ', 'function(', 'function() ', ' end', 'local ', 'return ', 'break', 'continue', 'rec(',
           'atomic ', 'readonly ', 'quit;', 'IsBound(', 'Unbind(', 'Info(', 'Assert(', 'TryNextMethod()',
           '1.5', '.5', '1e', '\n', ' ', 'x', 'true', '?']


def mep_verdicts(paths, jobs):
    """{path: 'ok' | 'LINE:COL: message'} from `mep-gap-lsp --gap-check`."""
    out = {}
    chunks = [paths[i:i + 200] for i in range(0, len(paths), 200)]

    def one(chunk):
        p = subprocess.run([str(BINARY), '--gap-check', *chunk], capture_output=True, timeout=600, env=ENV)
        res = {}
        for line in p.stdout.decode('utf-8', 'replace').splitlines():
            for path in chunk:
                if line.startswith(path + ': '):
                    res[path] = line[len(path) + 2:]
                    break
        for path in chunk:
            res.setdefault(path, 'crash' if p.returncode else 'missing')
        return res

    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for res in pool.map(one, chunks):
            out.update(res)
    return out


def gap_verdicts(paths, jobs):
    """{path: 'ok' | 'LINE:COL: message' | None (GAP aborted)}."""
    out = {}
    chunks = [paths[i::jobs] for i in range(jobs)]

    def one(chunk):
        if not chunk:
            return {}
        diags, verdicts = gap_check.run(chunk)
        first = {}
        for f, line, col, kind, msg in diags:
            if kind == 'error' and f not in first:
                first[f] = f'{line}:{col}: {msg}'
        res = {}
        for f in chunk:
            v = verdicts[f]
            res[f] = None if v == 'aborted' else 'ok' if v == 'accepted' else first.get(f, '?: rejected')
        return res

    with concurrent.futures.ThreadPoolExecutor(jobs) as pool:
        for res in pool.map(one, chunks):
            out.update(res)
    return out


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


def split(v):
    """'LINE:COL: msg' -> (LINE, COL, msg)."""
    line, col, msg = v.split(':', 2)
    return line, col, msg.strip()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--files', nargs='*', help='check these files instead of the corpus')
    ap.add_argument('--sample', type=int, default=0, help='check a random sample of N corpus files')
    ap.add_argument('--mutants', type=int, default=0, help='damaged copies per file')
    ap.add_argument('--no-originals', action='store_true', help='with --mutants: check only the damaged copies')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=10, help='examples to print per kind of disagreement')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    if not BINARY.exists():
        sys.exit(f'{BINARY} not built: cmake --build build/native --target mep-gap-lsp')

    rng = random.Random(args.seed)
    if args.files:
        files = args.files
    else:
        if not CORPUS.exists():
            sys.exit(f'no corpus at {CORPUS}: python3 tools/ts_grammar_diff/grammar_diff.py corpus gap')
        files = CORPUS.read_text().split()
        if args.sample:
            files = rng.sample(files, min(args.sample, len(files)))

    tmp = tempfile.TemporaryDirectory(prefix='gap-parse-check-')
    paths = [] if args.mutants and args.no_originals else list(files)
    for i, f in enumerate(files):
        data = Path(f).read_bytes()
        for m in range(args.mutants):
            p = Path(tmp.name) / f'{i}-{m}.g'
            p.write_bytes(mutate(data, rng))
            paths.append(str(p))

    gap = gap_verdicts(paths, args.jobs)
    mep = mep_verdicts(paths, args.jobs)

    kinds = collections.Counter()
    examples = collections.defaultdict(list)
    agree = col_agree = col_total = skipped = 0
    for p in paths:
        g, m = gap[p], mep[p]
        if g is None:
            skipped += 1
            continue
        if g == 'ok' and m == 'ok':
            agree += 1
            continue
        if g == 'ok' or m == 'ok' or m in ('crash', 'missing'):
            kind = f'gap {"accepts" if g == "ok" else "rejects"}, mep {m if m in ("crash", "missing") else "accepts" if m == "ok" else "rejects"}'
        else:
            gl, gc, gm = split(g)
            ml, mc, mm = split(m)
            if gl == ml and gm == mm:
                agree += 1
                if gc != '0':
                    col_total += 1
                    if gc == mc:
                        col_agree += 1
                    else:
                        examples['column differs'].append((p, g, m))
                        kinds['column differs (counted as agreeing)'] += 1
                continue
            kind = 'message differs' if gl == ml else 'line differs'
        kinds[kind] += 1
        examples[kind].append((p, g, m))

    total = len(paths) - skipped
    print(f'{agree} of {total} files agree with GAP ({100.0 * agree / max(total, 1):.2f}%)'
          f'{f"; {skipped} skipped (GAP aborted while reading)" if skipped else ""}')
    if col_total:
        print(f'columns: {col_agree} of {col_total} errors at GAP\'s column')
    oks = sum(1 for p in paths if gap[p] == 'ok')
    print(f'GAP accepts {oks}, rejects {total - oks}')
    if kinds:
        KEEP.mkdir(parents=True, exist_ok=True)
    for kind, n in kinds.most_common():
        print(f'\n{n:6}  {kind}')
        for p, g, m in examples[kind.replace(' (counted as agreeing)', '')][:args.show]:
            keep = p
            if p.startswith(tmp.name):
                keep = str(KEEP / Path(p).name)
                Path(keep).write_bytes(Path(p).read_bytes())
            print(f'        {keep}\n          gap: {g}\n          mep: {m}')
    return 0 if all(k.startswith('column') for k in kinds) else 1


if __name__ == '__main__':
    sys.exit(main())
