#!/usr/bin/env python3
"""Check mep's JavaScript parser (src/js_parse*) against acorn itself.

    tools/js_parse_check.py --node-modules DIR              # the whole corpus
    tools/js_parse_check.py --node-modules DIR --mutants 1
    tools/js_parse_check.py --node-modules DIR --files a.js b.jsx --show 20

DIR is a node_modules directory holding acorn/ and acorn-jsx/ (or set
JS_PARSE_NODE_MODULES). tools/js_parse_oracle.js runs acorn there; the
binary is mep-js-parse-test (--binary to point elsewhere).

Both sides parse every file the same way: .mjs as a module, .cjs as a
script, .js/.jsx as a module and, if that fails, as a script (the module
error is the verdict if both fail); JSX on for .jsx. Then:

  - acorn accepts: the ASTs (acorn's JSON, with locations) and comment
    lists must be identical;
  - acorn rejects: the message (with its "(line:col)") and pos must be the
    same.

`--mutants N` adds N damaged copies of each file (a slice deleted or
duplicated, or a token-ish string inserted; `--damage K` edits each copy K
times) to exercise the error paths.

The corpus is build/grammar-corpus/javascript/files.txt
(`python3 tools/ts_grammar_diff/grammar_diff.py corpus javascript`).
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
BINARY = REPO / 'build' / 'native' / 'mep-js-parse-test'
ORACLE = REPO / 'tools' / 'js_parse_oracle.js'
CORPUS = REPO / 'build' / 'grammar-corpus' / 'javascript' / 'files.txt'
KEEP = REPO / 'build' / 'js-parse-check'  # damaged copies that disagreed, kept to look at
ENV = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}

INSERTS = ['{', '}', '(', ')', '[', ']', ';', ',', '"', "'", '`', '${', '/', '/*', '*/', '//', '=>', '=', '.',
           '...', '?.', '??', '?', ':', '#', '@', '\\', '\\u', '\\u{', '0x', '1e', '08', '1n', '.5', 'function ',
           'class ', 'async ', 'await ', 'yield ', 'let ', 'const ', 'var ', 'return ', 'if ', 'else ', 'for ',
           'of ', 'in ', 'new ', 'import ', 'export ', 'default ', 'static ', 'get ', 'set ', 'super', 'this',
           'delete ', 'typeof ', 'with ', 'switch ', 'case ', 'try ', 'catch ', 'finally ', 'break ', 'continue ',
           'label: ', '<', '>', '</', '/>', '<>', '</>', '&amp;', '&', '|', '\n', '\r\n', ' ', 'x', '++',
           '--', '-->', '<!--', '"use strict";', '#x', 'arguments', 'eval', 'enum ', '\t']


def mode_for(path):
    ext = os.path.splitext(path)[1].lower()
    return {'.mjs': 'module', '.cjs': 'script'}.get(ext, 'auto'), ext == '.jsx'


def run_list(cmd, batch, tmpdir, tag):
    lst = Path(tmpdir) / f'{tag}.list'
    out = Path(tmpdir) / tag
    out.mkdir(exist_ok=True)
    lst.write_text(''.join(f'{m} {int(j)} {p}\n' for p, m, j, _ in batch))
    p = subprocess.run(cmd + [str(lst), str(out)], capture_output=True, timeout=1800, env=ENV)
    res = []
    for i in range(1, len(batch) + 1):
        f = out / f'{i}.json'
        try:
            res.append(json.loads(f.read_text(encoding='utf-8', errors='surrogatepass')))
        except (OSError, ValueError):
            res.append(None)
    return res, p


def norm(v):
    """Make == type-strict: bools and numbers stay apart, ints compare as floats."""
    if isinstance(v, bool):
        return ('bool', v)
    if isinstance(v, (int, float)):
        return float(v)
    return v


def first_diff(a, b, path='$'):
    if isinstance(a, dict) and isinstance(b, dict):
        for k in a:
            if k not in b:
                return f'{path}.{k} (only acorn)'
        for k in b:
            if k not in a:
                return f'{path}.{k} (only mep)'
        for k in a:
            d = first_diff(a[k], b[k], f'{path}.{k}')
            if d:
                return d
        return None
    if isinstance(a, list) and isinstance(b, list):
        if len(a) != len(b):
            return f'{path} (length {len(a)} vs {len(b)})'
        for i, (x, y) in enumerate(zip(a, b)):
            d = first_diff(x, y, f'{path}[{i}]')
            if d:
                return d
        return None
    if type(a) is bool or type(b) is bool:
        return None if type(a) is type(b) and a == b else f'{path}: {a!r} vs {b!r}'
    if norm(a) != norm(b):
        return f'{path}: {a!r} vs {b!r}'
    return None


def describe(path, j, src_cache):
    """A node's type at a JSON path, for the reports."""
    return path


def compare(acorn, mep):
    if acorn is None:
        return 'acorn-crash', 'no output', ''
    if mep is None:
        return 'crash', '', 'no output'
    if 'crash' in acorn:
        return 'acorn-crash', acorn['crash'][:200], ''
    a_err, m_err = 'error' in acorn, 'error' in mep
    if a_err and m_err:
        if acorn['error'] == mep['error'] and acorn['pos'] == mep['pos']:
            return 'agree', acorn['error'], mep['error']
        if acorn['error'].rsplit(' (', 1)[0] == mep['error'].rsplit(' (', 1)[0]:
            return 'position', f"{acorn['error']} @{acorn['pos']}", f"{mep['error']} @{mep['pos']}"
        return 'message', f"{acorn['error']} @{acorn['pos']}", f"{mep['error']} @{mep['pos']}"
    if a_err:
        return 'mep-ok', f"{acorn['error']} @{acorn['pos']}", 'ok'
    if m_err:
        return 'acorn-ok', 'ok', f"{mep['error']} @{mep['pos']}"
    d = first_diff(acorn['ast'], mep['ast'], '$.ast')
    if d:
        return 'ast', 'ok', d
    d = first_diff(acorn['comments'], mep['comments'], '$.comments')
    if d:
        return 'comments', 'ok', d
    return 'agree', 'ok', 'ok'


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


def find_node():
    node = shutil.which('node')
    if node:
        return node
    p = subprocess.run(['nix', 'shell', 'nixpkgs#nodejs', '--command', 'sh', '-c', 'command -v node'],
                       capture_output=True, text=True)
    return p.stdout.strip() or None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--files', nargs='*', help='check these files instead of the corpus')
    ap.add_argument('--sample', type=int, default=0, help='check a random sample of N corpus files')
    ap.add_argument('--mutants', type=int, default=0, help='damaged copies per file')
    ap.add_argument('--originals', action=argparse.BooleanOptionalAction, default=True,
                    help='check the undamaged files too (--no-originals: mutants only)')
    ap.add_argument('--damage', type=int, default=1, help='edits per mutant')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=10, help='examples to print per kind of disagreement')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--batch', type=int, default=60)
    ap.add_argument('--binary', default=str(BINARY))
    ap.add_argument('--node', default=None, help='node executable (default: PATH, else nixpkgs#nodejs)')
    ap.add_argument('--node-modules', default=os.environ.get('JS_PARSE_NODE_MODULES'),
                    help='directory with acorn/ and acorn-jsx/')
    args = ap.parse_args()
    binary = Path(args.binary)
    if not binary.exists():
        sys.exit(f'{binary} not built: cmake --build build/native --target mep-js-parse-test')
    if not args.node_modules or not (Path(args.node_modules) / 'acorn').is_dir():
        sys.exit('need --node-modules DIR (or JS_PARSE_NODE_MODULES) with acorn/ and acorn-jsx/ in it')
    node = args.node or find_node()
    if not node:
        sys.exit('no node: pass --node')

    rng = random.Random(args.seed)
    if args.files:
        files = args.files
    else:
        if not CORPUS.exists():
            sys.exit(f'no corpus at {CORPUS}: python3 tools/ts_grammar_diff/grammar_diff.py corpus javascript')
        files = [l for l in CORPUS.read_text().splitlines() if l]
        if args.sample and args.sample < len(files):
            files = rng.sample(files, args.sample)

    tmp = tempfile.TemporaryDirectory(prefix='jscheck-')
    work = []  # (path, mode, jsx, label)
    for i, f in enumerate(files):
        mode, jsx = mode_for(f)
        if args.originals:
            work.append((f, mode, jsx, f))
        if args.mutants:
            try:
                data = Path(f).read_bytes()
            except OSError:
                continue
            ext = os.path.splitext(f)[1] or '.js'
            for j in range(args.mutants):
                m = Path(tmp.name) / f'm{i}_{j}{ext}'
                md = data
                for _ in range(args.damage):
                    md = mutate(md, rng)
                m.write_bytes(md)
                work.append((str(m), mode, jsx, f'{f} (mutant {j})'))

    batches = [work[k:k + args.batch] for k in range(0, len(work), args.batch)]
    tally = collections.Counter()
    examples = collections.defaultdict(list)

    def run_batch(item):
        bi, batch = item
        d = Path(tmp.name) / f'b{bi}'
        d.mkdir()
        mep, _ = run_list([str(binary), '--json-list'], batch, d, 'mep')
        for k, r in enumerate(mep):
            if r is None:  # a crash ends the batch: redo the rest one at a time
                one, _ = run_list([str(binary), '--json-list'], [batch[k]], d, f'mep{k}')
                mep[k] = one[0]
        acorn, _ = run_list([node, str(ORACLE), args.node_modules, '--list'], batch, d, 'acorn')
        out = [(label, p) + compare(a, m) for (p, _, _, label), a, m in zip(batch, acorn, mep)]
        shutil.rmtree(d, ignore_errors=True)
        return out

    done = 0
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        for results in ex.map(run_batch, enumerate(batches)):
            for label, p, kind, a, m in results:
                tally[('agree: ok' if a == 'ok' else 'agree: error') if kind == 'agree' else kind] += 1
                if kind != 'agree' and len(examples[kind]) < args.show:
                    keep = p
                    if p != label:
                        KEEP.mkdir(parents=True, exist_ok=True)
                        keep = str(KEEP / Path(p).name)
                        try:
                            Path(keep).write_bytes(Path(p).read_bytes())
                        except OSError:
                            pass
                    examples[kind].append((label, keep, a, m))
            done += len(results)
            print(f'\r{done}/{len(work)}', end='', file=sys.stderr, flush=True)
    print(file=sys.stderr)

    total = sum(tally.values())
    print(f'{total} files ({len(files)} originals, {args.mutants} mutants each)')
    for kind, n in tally.most_common():
        print(f'  {kind:24} {n:7}  {100.0 * n / max(total, 1):6.2f}%')
    for kind, exs in examples.items():
        print(f'\n== {kind}')
        for label, path, a, m in exs:
            print(f'  {label}' + (f'  [{path}]' if path != label else ''))
            print(f'    acorn: {a}')
            print(f'    mep:   {m}')
    return 1 if any(not k.startswith('agree') for k in tally) else 0


if __name__ == '__main__':
    sys.exit(main())
