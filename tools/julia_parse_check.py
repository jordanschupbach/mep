#!/usr/bin/env python3
"""Check mep's Julia parser (src/julia_parse*.cpp) against Julia's own.

    python3 tools/julia_parse_check.py                       # the whole corpus
    python3 tools/julia_parse_check.py --sample 300 --mutants 3 --seed 1
    python3 tools/julia_parse_check.py --files a.jl b.jl --show 20

For every file three things are compared:
  tree   our green tree vs JuliaSyntax 0.4.10's (the version Runic.jl 1.5
         uses): every node's kind, flags, span and children, trivia included;
  diag   our diagnostics vs Julia 1.12's parser (Base.JuliaSyntax 1.0.2, what
         `Meta.parseall` reports): the whole (first, last, level, message) list;
  first  just the first error diagnostic (message + byte range).
`--mutants N` adds N damaged copies of each file (a token or run deleted, a
delimiter inserted, bytes swapped) to exercise the error paths.

The oracle is tools/julia_parse_oracle.jl, run by a julia whose environment
provides JuliaSyntax 0.4.10 (directly or through Runic): $JULIA_ORACLE, default
`julia`. Our side is $MEP_JULIA_PARSE_TEST (default
build/native/mep-julia-parse-test), run with `--tree` / `--diag`.

The corpus is build/grammar-corpus/julia/files.txt.
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
BINARY = Path(os.environ.get('MEP_JULIA_PARSE_TEST', REPO / 'build' / 'native' / 'mep-julia-parse-test'))
JULIA = os.environ.get('JULIA_ORACLE', 'julia')
ORACLE = REPO / 'tools' / 'julia_parse_oracle.jl'
CORPUS = REPO / 'build' / 'grammar-corpus' / 'julia' / 'files.txt'
KEEP = REPO / 'build' / 'julia-parse-check'  # damaged copies that disagreed, kept to look at
ENV = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}

INSERTS = ['(', ')', '[', ']', '{', '}', '"', '"""', "'", '`', ',', ';', ':', '::', '=', '==', '->', '.', '..',
           '...', '$', '$(', '@', '#', '#=', '=#', '\\', ' ', '\n', 'end', ' end ', 'end\n', 'begin ', 'function ',
           'if ', 'elseif ', 'else ', 'for ', 'while ', 'do ', 'let ', 'struct ', 'module ', 'macro ', 'quote ',
           'try ', 'catch ', 'finally ', 'return ', 'import ', 'using ', 'export ', 'const ', 'global ', 'where ',
           'in ', 'isa ', '+', '-', '*', '^', '<:', '?', '&&', '||', '|>', '.+', 'x', '1', '1.', '0x', '1e',
           '−', '·', '​', '‮', '\xff']
TOKEN_RE = re.compile(rb'[A-Za-z_][A-Za-z_0-9!]*|[0-9][0-9.e_]*|\s+|.', re.S)


def mutate(data, rng):
    if not data:
        return rng.choice(INSERTS).encode('utf-8', 'surrogateescape')
    op = rng.randrange(4)
    k = rng.randrange(len(data))
    if op == 0:  # delete a token
        m = TOKEN_RE.match(data, k)
        return data[:k] + data[m.end():] if m else data[:k] + data[k + 1:]
    if op == 1:  # delete a run
        return data[:k] + data[k + rng.randint(1, 40):]
    if op == 2:  # insert a delimiter / keyword
        ins = rng.choice(INSERTS)
        b = ins.encode('latin-1') if ins == '\xff' else ins.encode('utf-8')
        return data[:k] + b + data[k:]
    j = rng.randrange(len(data))  # swap two bytes
    b = bytearray(data)
    b[k], b[j] = b[j], b[k]
    return bytes(b)


def split_dump(text):
    """`### N` sections -> {N: [lines]}"""
    out = {}
    cur = None
    for line in text.split('\n'):
        if line.startswith('### '):
            cur = int(line[4:])
            out[cur] = []
        elif cur is not None and line:
            out[cur].append(line)
    return out


def run_ours(mode, paths):
    """Returns {index: lines} or None when the binary crashed / hung."""
    try:
        p = subprocess.run([str(BINARY), mode, *map(str, paths)], capture_output=True, timeout=300, env=ENV)
    except subprocess.TimeoutExpired:
        return None
    if p.returncode != 0:
        return None
    return split_dump(p.stdout.decode('utf-8', 'surrogateescape'))


def run_oracle(paths, outdir, jobs):
    chunks = [paths[i::jobs] for i in range(jobs)]
    procs = []
    for ci, chunk in enumerate(chunks):
        if not chunk:
            continue
        d = Path(outdir) / f'c{ci}'
        d.mkdir(parents=True, exist_ok=True)
        lst = d / 'list.txt'
        lst.write_text(''.join(str(p) + '\n' for p in chunk))
        procs.append((ci, chunk, d, subprocess.Popen([JULIA, '--startup-file=no', str(ORACLE), str(d), str(lst)],
                                                     env=ENV)))
    result = {}
    for ci, chunk, d, proc in procs:
        proc.wait()
        for n, path in enumerate(chunk):
            got = {}
            for ext in ('tree', 'diag04', 'diag'):
                f = d / f'{n}.{ext}'
                got[ext] = f.read_text('utf-8', 'surrogateescape').splitlines() if f.exists() else ['CRASH missing']
            result[str(path)] = got
    return result


def ours_for(paths, jobs):
    """Run our binary over all paths in batches; {path: {'tree':..., 'diag':...}} plus crashes."""
    batches = [paths[k:k + 100] for k in range(0, len(paths), 100)]
    res = {}
    crashed = []

    def one(batch):
        out = {}
        for mode, key in (('--tree', 'tree'), ('--diag', 'diag')):
            r = run_ours(mode, batch)
            if r is None:
                r = {}
                for i, p in enumerate(batch):
                    s = run_ours(mode, [p])
                    if s is None:
                        crashed.append(p)
                    else:
                        r[i] = s.get(0, [])
            for i, p in enumerate(batch):
                out.setdefault(p, {})[key] = r.get(i)
        return out

    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        for out in ex.map(one, batches):
            res.update(out)
    return res, sorted(set(crashed))


def first_error(lines):
    for l in lines:
        f = l.split('\t')
        if len(f) >= 4 and f[2] == 'error':
            return l
    return None


def first_diff(a, b):
    for i, (x, y) in enumerate(zip(a, b)):
        if x != y:
            return i, x, y
    if len(a) != len(b):
        i = min(len(a), len(b))
        return i, (a[i] if i < len(a) else '<end>'), (b[i] if i < len(b) else '<end>')
    return None


def tree_offset(lines, upto):
    """Byte offset of node `upto` in a preorder dump (sum of leaf spans before it)."""
    off = 0
    for l in lines[:upto]:
        f = l.split('\t')
        if len(f) == 5 and f[4] == 'L':
            off += int(f[3])
    return off


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--files', nargs='*', help='check these files instead of the corpus')
    ap.add_argument('--sample', type=int, default=0, help='check a random sample of N corpus files')
    ap.add_argument('--mutants', type=int, default=0, help='damaged copies per file')
    ap.add_argument('--no-originals', action='store_true', help='with --mutants, check only the damaged copies')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=8, help='examples to print per kind of disagreement')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    args = ap.parse_args()
    if not BINARY.exists():
        sys.exit(f'{BINARY} not built (set MEP_JULIA_PARSE_TEST)')

    rng = random.Random(args.seed)
    if args.files:
        files = args.files
    else:
        if not CORPUS.exists():
            sys.exit(f'no corpus at {CORPUS}')
        files = [l for l in CORPUS.read_text().splitlines() if l]
        if args.sample and args.sample < len(files):
            files = rng.sample(files, args.sample)

    tmp = tempfile.TemporaryDirectory(prefix='jlcheck-')
    work = []  # (path, label)
    for i, f in enumerate(files):
        if not (args.mutants and args.no_originals):
            work.append((str(f), str(f)))
        if args.mutants:
            try:
                data = Path(f).read_bytes()
            except OSError:
                continue
            for j in range(args.mutants):
                m = Path(tmp.name) / f'm{i}_{j}.jl'
                m.write_bytes(mutate(data, rng))
                work.append((str(m), f'{f} (mutant {j})'))
    paths = [p for p, _ in work]

    print(f'running oracle on {len(paths)} files ...', file=sys.stderr)
    oracle = run_oracle(paths, Path(tmp.name) / 'oracle', min(args.jobs, 16))
    print('running mep ...', file=sys.stderr)
    ours, crashed = ours_for(paths, args.jobs)

    tally = collections.Counter()
    examples = collections.defaultdict(list)
    for path, label in work:
        o = oracle[path]
        m = ours.get(path, {})
        if path in crashed or m.get('tree') is None or m.get('diag') is None:
            tally['crash'] += 1
            continue
        if o['tree'][:1] and o['tree'][0].startswith('CRASH'):
            tally['oracle crash (skipped)'] += 1
            continue
        tally['files'] += 1
        d = first_diff(o['tree'], m['tree'])
        if d is None:
            tally['tree identical'] += 1
        else:
            tally['tree differs'] += 1
            if len(examples['tree']) < args.show:
                i, x, y = d
                examples['tree'].append((label, path, f'node {i} at byte {tree_offset(o["tree"], i) + 1}',
                                         x, y))
        od = o['diag']
        if od[:1] and od[0].startswith('CRASH'):
            tally['diag oracle crash'] += 1
            continue
        if od == m['diag']:
            tally['diag identical'] += 1
        else:
            tally['diag differs'] += 1
            if len(examples['diag']) < args.show:
                i, x, y = first_diff(od, m['diag'])
                examples['diag'].append((label, path, f'diagnostic {i}', x, y))
        if first_error(od) == first_error(m['diag']):
            tally['first error identical'] += 1
        else:
            tally['first error differs'] += 1
            if len(examples['first']) < args.show:
                examples['first'].append((label, path, 'first error', first_error(od), first_error(m['diag'])))

    n = tally['files']
    print(f'{len(work)} files ({len(files)} originals, {args.mutants} mutants each); '
          f'{tally["crash"]} crashes, {tally["oracle crash (skipped)"]} oracle crashes')
    for key in ('tree identical', 'tree differs', 'diag identical', 'diag differs', 'first error identical',
                'first error differs', 'diag oracle crash'):
        if key in tally or key.endswith('identical'):
            print(f'  {key:24} {tally[key]:7}  {100.0 * tally[key] / max(n, 1):6.2f}%')
    for p in crashed[:args.show]:
        print(f'CRASH {p}')
    for kind, exs in examples.items():
        print(f'\n== {kind}')
        for label, path, where, x, y in exs:
            keep = path
            if path != label:
                KEEP.mkdir(parents=True, exist_ok=True)
                keep = str(KEEP / Path(path).name)
                shutil.copyfile(path, keep)
            print(f'  {label}' + (f'  [{keep}]' if keep != label else '') + f'  {where}')
            print(f'    julia: {x}')
            print(f'    mep:   {y}')
    bad = tally['crash'] + tally['tree differs'] + tally['diag differs']
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
