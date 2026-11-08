#!/usr/bin/env python3
"""Check mep's Rust parser (src/rust_parse*) against rustc's own:
`rustc -Zparse-crate-root-only` (tools/ts_grammar_diff/rust_check.py).

    just rust-parse-check                        # the whole corpus
    just rust-parse-check --sample 2000 --mutants 3
    just rust-parse-check --files a.rs --show 20

For every file both say "ok" or a first error `LINE:COL: message`. Reported:
accept/reject agreement, then for files both reject whether the first error
is on the same line, at the same line:col, and with the same message.
`--mutants N` adds N damaged copies of each file (one token deleted, or a
delimiter / keyword / quote inserted at a token boundary) to exercise the
error paths; the copy keeps the original's edition.

rustc's answers are cached by (content hash, edition) under
build/rust-parse-work/oracle-cache.json, so reruns only pay for mep.

The corpus is build/grammar-corpus/rust/files.txt
(`python3 tools/ts_grammar_diff/grammar_diff.py corpus rust`).
"""

import argparse
import collections
import concurrent.futures
import hashlib
import json
import os
import random
import re
import subprocess
import sys
import threading
import time
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools' / 'ts_grammar_diff'))
import rust_check  # noqa: E402  (the oracle; its edition() rules are ours too)

DEFAULT_BINARY = REPO / 'build' / 'rust-parse' / 'mep-rust-parse-test'
CORPUS = REPO / 'build' / 'grammar-corpus' / 'rust' / 'files.txt'
WORK = REPO / 'build' / 'rust-parse-work'
CACHE = WORK / 'oracle-cache.json'
MUTANT_DIR = WORK / 'mutants'
ENV = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}

INSERTS = ['{', '}', '(', ')', '[', ']', '<', '>', ';', ',', ':', '::', '=', '=>', '->', '"', "'", '#', '#[', '!',
           '&', '|', '..', '?', '.', '@', 'fn ', 'let ', 'struct ', 'impl ', 'mut ', 'pub ', 'as ', 'if ', 'else ',
           'match ', 'in ', 'for ', 'where ', 'move ', 'async ', 'dyn ', 'const ', 'unsafe ', 'r#"', 'b\'', '/*',
           '//', '0x', '1e', '1.', "'a ", 'x ', '\n']

TOKEN = re.compile(rb"[A-Za-z_][A-Za-z0-9_]*|[0-9][A-Za-z0-9_.]*|\"(?:[^\"\\\\]|\\\\.)*\"|'[^'\\\\\n]'|::|=>|->|\.\.=|\.\.|[^\s\w]")

_cache_lock = threading.Lock()


def load_cache():
    try:
        return json.loads(CACHE.read_text())
    except (OSError, ValueError):
        return {}


def save_cache(cache):
    WORK.mkdir(parents=True, exist_ok=True)
    tmp = CACHE.with_suffix('.tmp')
    tmp.write_text(json.dumps(cache))
    tmp.replace(CACHE)


def oracle(path, edition, data, cache):
    key = hashlib.sha1(data).hexdigest() + ':' + edition
    with _cache_lock:
        if key in cache:
            return cache[key]
    p = subprocess.run(['rustc', '-Zparse-crate-root-only', '--edition', edition, '--crate-type', 'lib',
                        '--error-format', 'human', '--color', 'never', '-o', os.devnull, str(path)],
                       capture_output=True, text=True, errors='replace', env=dict(ENV, RUSTC_BOOTSTRAP='1'),
                       timeout=120)
    if p.returncode == 0:
        verdict = 'ok'
    else:
        m = rust_check.ERROR.search(p.stderr)
        if m:
            verdict = f'{m.group(3)}:{m.group(4)}: {m.group(1)}'
        else:
            first = p.stderr.strip().splitlines()[:1]
            verdict = '0:0: ' + (first[0] if first else f'rustc exited {p.returncode}')
    with _cache_lock:
        cache[key] = verdict
    return verdict


def mep_batch(binary, edition, paths):
    """{path: verdict}; a crash or hang shows as 'CRASH ...'."""
    try:
        p = subprocess.run([str(binary), '--edition', edition, '--check', *map(str, paths)], capture_output=True,
                           timeout=600, env=ENV)
    except subprocess.TimeoutExpired:
        if len(paths) == 1:
            return {str(paths[0]): 'CRASH timeout'}
        p = None
    if p is None or p.returncode not in (0, 1):
        if len(paths) == 1:
            return {str(paths[0]): f'CRASH exit {p.returncode if p else "timeout"}'}
        half = len(paths) // 2
        out = mep_batch(binary, edition, paths[:half])
        out.update(mep_batch(binary, edition, paths[half:]))
        return out
    out = {str(x): 'ok' for x in paths}
    for line in p.stdout.decode('utf-8', 'replace').splitlines():
        for x in paths:
            sx = str(x)
            if line.startswith(sx + ':'):
                out[sx] = line[len(sx) + 1:]
                break
    return out


def mutate(data, rng):
    toks = [m.span() for m in TOKEN.finditer(data)]
    if not toks:
        return data + rng.choice(INSERTS).encode()
    a, b = toks[rng.randrange(len(toks))]
    if rng.random() < 0.5:
        return data[:a] + data[b:]
    ins = rng.choice(INSERTS).encode()
    pos = a if rng.random() < 0.5 else b
    return data[:pos] + ins + data[pos:]


def classify(rustc, mep):
    # rustc errors without a location come back as `0:0: error[E..]: message`.
    rustc = re.sub(r'^0:0: error(?:\[E\d+\])?: ', '0:0: ', rustc)
    rustc = re.sub(r"^(\d+:\d+: )couldn't read `[^`]*`: ", r'\1', rustc)
    if rustc == mep:
        return 'exact'
    if mep.startswith('CRASH'):
        return 'crash'
    if rustc == 'ok':
        return 'mep-rejects-ok'
    if mep == 'ok':
        return 'mep-accepts-bad'
    rm = re.match(r'(\d+):(\d+): (.*)$', rustc)
    mm = re.match(r'(\d+):(\d+): (.*)$', mep)
    if not rm or not mm:
        return 'other'
    if rm.group(1) == mm.group(1) and rm.group(2) == mm.group(2):
        return 'same-pos'  # message differs
    if rm.group(3) == mm.group(3):
        return 'same-msg'  # position differs
    if rm.group(1) == mm.group(1):
        return 'same-line'
    return 'differ'


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--files', nargs='*', help='check these files instead of the corpus')
    ap.add_argument('--sample', type=int, default=0, help='a random sample of the corpus')
    ap.add_argument('--mutants', type=int, default=0, help='damaged copies per file')
    ap.add_argument('--no-originals', action='store_true', help='with --mutants: only check the copies')
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=10, help='disagreements to print per class')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    ap.add_argument('--binary', default=str(DEFAULT_BINARY))
    ap.add_argument('--out', help='write every disagreement here (TSV)')
    args = ap.parse_args()

    binary = Path(args.binary)
    if not binary.exists():
        sys.exit(f'{binary} missing: cmake --build build/rust-parse --target mep-rust-parse-test')
    if args.files:
        files = [Path(f) for f in args.files]
    else:
        files = [Path(x) for x in CORPUS.read_text().split('\n') if x]
    rng = random.Random(args.seed)
    if args.sample and args.sample < len(files):
        files = rng.sample(files, args.sample)

    cache = load_cache()
    # (path, edition, data, label)
    jobs = []
    for f in files:
        try:
            data = f.read_bytes()
        except OSError:
            continue
        ed = rust_check.edition(str(f))
        if not args.no_originals or not args.mutants:
            jobs.append((f, ed, data, 'file'))
        if args.mutants:
            MUTANT_DIR.mkdir(parents=True, exist_ok=True)
            for i in range(args.mutants):
                mdata = mutate(data, rng)
                h = hashlib.sha1(mdata).hexdigest()[:16]
                mp = MUTANT_DIR / f'{h}.rs'
                if not mp.exists():
                    mp.write_bytes(mdata)
                jobs.append((mp, ed, mdata, 'mutant'))

    t0 = time.time()
    # mep, batched per edition
    by_ed = collections.defaultdict(list)
    for j in jobs:
        by_ed[j[1]].append(j[0])
    mep = {}
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        futs = []
        for ed, paths in by_ed.items():
            for i in range(0, len(paths), 200):
                futs.append(ex.submit(mep_batch, binary, ed, paths[i:i + 200]))
        for fu in futs:
            mep.update(fu.result())
    t_mep = time.time() - t0

    t1 = time.time()
    with concurrent.futures.ThreadPoolExecutor(args.jobs) as ex:
        verdicts = list(ex.map(lambda j: oracle(j[0], j[1], j[2], cache), jobs))
    save_cache(cache)
    t_rustc = time.time() - t1

    tally = {'file': collections.Counter(), 'mutant': collections.Counter()}
    stats = {'file': collections.Counter(), 'mutant': collections.Counter()}
    examples = collections.defaultdict(list)
    rows = []
    for j, rv in zip(jobs, verdicts):
        mv = mep.get(str(j[0]), 'CRASH missing')
        c = classify(rv, mv)
        tally[j[3]][c] += 1
        st = stats[j[3]]
        st['n'] += 1
        if c not in ('mep-rejects-ok', 'mep-accepts-bad', 'crash'):
            st['agree'] += 1
        if rv != 'ok' and mv != 'ok' and not mv.startswith('CRASH'):
            st['both'] += 1
            if c == 'exact':
                st['exact'] += 1
            if c in ('exact', 'same-pos', 'same-line'):
                st['line'] += 1
            if c in ('exact', 'same-msg'):
                st['msg'] += 1
        if c != 'exact':
            examples[(j[3], c)].append((str(j[0]), j[1], rv, mv))
            rows.append(f'{j[3]}\t{c}\t{j[0]}\t{j[1]}\t{rv}\t{mv}')

    def pct(a, b):
        return f'{100.0 * a / b:.2f}%' if b else '-'

    for label in ('file', 'mutant'):
        st = stats[label]
        if not st['n']:
            continue
        print(f'== {label}s: {st["n"]}')
        print(f'   accept/reject agree: {st["agree"]} ({pct(st["agree"], st["n"])})')
        print(f'   both reject: {st["both"]}: same first error {st["exact"]} ({pct(st["exact"], st["both"])}), '
              f'same line {st["line"]} ({pct(st["line"], st["both"])}), same message {st["msg"]} '
              f'({pct(st["msg"], st["both"])})')
        for k, v in sorted(tally[label].items()):
            print(f'   {k}: {v}')
    print(f'mep {t_mep:.1f}s, rustc {t_rustc:.1f}s ({len(jobs)} files)')

    for (label, c), ex in sorted(examples.items()):
        print(f'\n-- {label} {c} ({len(ex)})')
        for path, ed, rv, mv in ex[:args.show]:
            print(f'  {path} [{ed}]\n    rustc: {rv}\n    mep:   {mv}')
    if args.out:
        Path(args.out).write_text('\n'.join(rows) + '\n')
    bad = sum(t['mep-rejects-ok'] + t['mep-accepts-bad'] + t['crash'] for t in tally.values())
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
