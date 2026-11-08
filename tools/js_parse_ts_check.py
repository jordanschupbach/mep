#!/usr/bin/env python3
"""Check mep's TypeScript parser (src/js_parse_ts*.cpp) against TypeScript.

    tools/js_parse_ts_check.py                       # the TS + TSX corpus
    tools/js_parse_ts_check.py --conformance DIR...  # TypeScript's tests/cases/{conformance,compiler}
    tools/js_parse_ts_check.py --trees 300           # + tree shapes vs typescript-estree
    tools/js_parse_ts_check.py --mutants 1           # + damaged copies of every file
    tools/js_parse_ts_check.py --files a.ts b.tsx --show 20

Acceptance: for every file, does ts.createSourceFile report parse
diagnostics (what tsc calls syntax errors), and does mep-js-parse-test
(Options::typescript) report an error? The verdicts:

    accepted   both accept
    same-pos   both reject, the first error at the same offset
    other-pos  both reject, at different offsets
    over       mep rejects, tsc accepts   (a bug)
    under      mep accepts, tsc rejects   (mostly: tsc's parser reports a
               few grammar errors acorn's style leaves out)

Trees (--trees N): for N files both accept, every node of mep's tree and of
typescript-estree's as "TYPE START END", compared as multisets.

The script kind comes from the extension as in tsc (.tsx: JSX; .d.ts: a
declaration file; .ts/.mts/.cts). Conformance tests are split at their
`// @filename:` directives into one unit per file (only the TS ones kept).

The oracle is tools/js_parse_ts_oracle.js; NODE_PATH must hold typescript
and @typescript-eslint/typescript-estree (+ visitor-keys), e.g. a project's
node_modules (--node-path). The corpus is build/grammar-corpus/{typescript,
tsx}/files.txt (`python3 tools/ts_grammar_diff/grammar_diff.py corpus typescript`).
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
BINARY = REPO / 'build' / 'native' / 'mep-js-parse-test'
ORACLE = REPO / 'tools' / 'js_parse_ts_oracle.js'
CORPORA = [REPO / 'build' / 'grammar-corpus' / 'typescript' / 'files.txt',
           REPO / 'build' / 'grammar-corpus' / 'tsx' / 'files.txt']

INSERTS = ['{', '}', '(', ')', '[', ']', ';', ',', '"', "'", '`', '${', '/', '/*', '*/', '//', '=>', '=', '.',
           '...', '?.', '??', '?', ':', '@', '<', '>', '</', '/>', '<>', '>>', '&', '|', '!', '\n', ' ', 'x',
           'function ', 'class ', 'async ', 'await ', 'yield ', 'let ', 'const ', 'return ', 'if ', 'new ',
           'import ', 'export ', 'default ', 'static ', 'get ', 'set ', 'this', 'typeof ', 'keyof ', 'as ',
           'satisfies ', 'interface ', 'type ', 'enum ', 'namespace ', 'module ', 'declare ', 'abstract ',
           'public ', 'private ', 'readonly ', 'extends ', 'implements ', 'infer ', 'is ', 'asserts ',
           'unique symbol', '<T>', '<T,>', 'number', ': string', '?:', '!:', '[]', '[K in T]', 'override ']


def lang_of(path):
    p = path.lower()
    if p.endswith('.tsx'):
        return 'tsx'
    if re.search(r'\.d\.[mc]?ts$', p):
        return 'dts'
    if re.search(r'\.[mc]?ts$', p):
        return 'ts'
    return None


def split_conformance(dirs, outdir):
    """One unit per `// @filename:` section of each test (TS units only)."""
    units = []
    directive = re.compile(r'^\s*//\s*@filename\s*:\s*(.+?)\s*$', re.I | re.M)
    files = sorted(f for d in dirs for f in Path(d).rglob('*') if f.is_file() and lang_of(f.name))
    for f in files:
        text = f.read_bytes().decode('utf-8', 'replace')
        marks = list(directive.finditer(text))
        sections = []
        if not marks:
            sections.append((f.name, text))
        else:
            head = text[:marks[0].start()]
            for i, m in enumerate(marks):
                body = text[m.end():marks[i + 1].start() if i + 1 < len(marks) else len(text)]
                # the options before the first @filename belong to every unit; keep them as the
                # first unit's leading comments (they are comments either way)
                sections.append((m.group(1), (head if i == 0 else '') + body))
        for name, body in sections:
            lang = lang_of(name)
            if not lang:
                continue
            ext = {'tsx': '.tsx', 'dts': '.d.ts', 'ts': '.ts'}[lang]
            p = Path(outdir) / f'{len(units)}{ext}'
            p.write_text(body, encoding='utf-8')
            units.append((lang, str(p), f'{f}:{name}'))
    return units


def run_chunks(cmd_for_chunk, items, jobs):
    chunks = [items[i:i + 200] for i in range(0, len(items), 200)]
    results = [None] * len(chunks)
    with concurrent.futures.ThreadPoolExecutor(jobs) as ex:
        futs = {ex.submit(cmd_for_chunk, c): i for i, c in enumerate(chunks)}
        for fut in concurrent.futures.as_completed(futs):
            results[futs[fut]] = fut.result()
    return [line for r in results for line in r]


def write_list(items, tmp):
    fd, path = tempfile.mkstemp(suffix='.list', dir=tmp)
    with os.fdopen(fd, 'w', encoding='utf-8') as f:
        for lang, p, _ in items:
            f.write(f'{lang} {p}\n')
    return path


def errors_mep(binary, items, tmp, jobs, env):
    def one(chunk):
        lst = write_list(chunk, tmp)
        try:
            r = subprocess.run([str(binary), '--errors-list', lst], capture_output=True, env=env, timeout=600)
            lines = r.stdout.decode('utf-8', 'replace').splitlines()
        except subprocess.TimeoutExpired:
            lines = []
        if len(lines) != len(chunk):
            # a crash or a hang: redo one file at a time to find it
            lines = []
            for item in chunk:
                l1 = write_list([item], tmp)
                try:
                    r = subprocess.run([str(binary), '--errors-list', l1], capture_output=True, env=env, timeout=60)
                    out = r.stdout.decode('utf-8', 'replace').splitlines()
                    lines.append(out[0] if out and r.returncode == 0 else f'crash {r.returncode}')
                except subprocess.TimeoutExpired:
                    lines.append('crash timeout')
        return lines
    return run_chunks(one, items, jobs)


def errors_tsc(items, tmp, jobs, env):
    def one(chunk):
        lst = write_list(chunk, tmp)
        r = subprocess.run(['node', str(ORACLE), '--errors', lst], capture_output=True, env=env, timeout=900)
        lines = r.stdout.decode('utf-8', 'replace').splitlines()
        if len(lines) != len(chunk):
            sys.exit('oracle failed: ' + r.stderr.decode()[-2000:])
        return lines
    return run_chunks(one, items, jobs)


def pos_of(line):
    return int(line.split()[1])


def compare(items, mine, theirs):
    cats = collections.defaultdict(list)
    for item, m, t in zip(items, mine, theirs):
        if m.startswith('crash'):
            cats['crash'].append((item, m, t))
        elif m == 'ok' and t == 'ok':
            cats['accepted'].append((item, m, t))
        elif m != 'ok' and t != 'ok':
            cats['same-pos' if pos_of(m) == pos_of(t) else 'other-pos'].append((item, m, t))
        elif m != 'ok':
            cats['over'].append((item, m, t))
        else:
            cats['under'].append((item, m, t))
    return cats


def report(title, items, cats, show):
    n = len(items)
    print(f'== {title}: {n} files')
    for k in ['accepted', 'same-pos', 'other-pos', 'over', 'under', 'crash']:
        v = cats.get(k, [])
        print(f'  {k:10} {len(v):6}  {100.0 * len(v) / max(n, 1):6.2f}%')
    rejected = len(cats.get('same-pos', [])) + len(cats.get('other-pos', []))
    agree = len(cats.get('accepted', [])) + len(cats.get('same-pos', []))
    print(f'  acceptance agrees on {len(cats.get("accepted", [])) + rejected}/{n}; '
          f'accepted-accepted + rejected-at-same-position {agree}/{n}')
    for k in ['crash', 'over', 'under', 'other-pos']:
        for (lang, p, label), m, t in cats.get(k, [])[:show]:
            print(f'  [{k}] {label}\n      mep: {m}\n      tsc: {t}')


def damage(text, rng, edits):
    for _ in range(edits):
        if not text:
            break
        i = rng.randrange(len(text))
        op = rng.random()
        if op < 0.3:
            j = min(len(text), i + rng.randint(1, 20))
            text = text[:i] + text[j:]
        elif op < 0.45:
            j = min(len(text), i + rng.randint(1, 30))
            text = text[:i] + text[i:j] + text[i:]
        else:
            text = text[:i] + rng.choice(INSERTS) + text[i:]
    return text


def shapes(binary, items, tmp, jobs, env):
    mine_dir = tempfile.mkdtemp(prefix='mine-', dir=tmp)
    their_dir = tempfile.mkdtemp(prefix='estree-', dir=tmp)
    lst = write_list(items, tmp)
    subprocess.run([str(binary), '--shape-list', lst, mine_dir], env=env, check=False, timeout=3600)
    subprocess.run(['node', str(ORACLE), '--shape-list', lst, their_dir], env=env, check=False, timeout=3600)
    return mine_dir, their_dir


def compare_trees(items, mine_dir, their_dir, show, ignore=()):
    same = 0
    skipped = 0
    diff_types = collections.Counter()
    examples = []
    for i, item in enumerate(items, 1):
        def read(d):
            try:
                return Path(d, f'{i}.txt').read_text(encoding='utf-8').splitlines()
            except OSError:
                return ['error missing']
        m = read(mine_dir)
        t = read(their_dir)
        if any(x.startswith('error') for x in t):
            skipped += 1
            continue
        skip = ('Program ', 'error') + tuple(k + ' ' for k in ignore)
        cm = collections.Counter(x for x in m if not x.startswith(skip))
        ct = collections.Counter(x for x in t if not x.startswith(skip))
        if cm == ct:
            same += 1
            continue
        only_m = cm - ct
        only_t = ct - cm
        kinds = set()
        for x in list(only_m) + list(only_t):
            kinds.add(x.split()[0])
        for k in kinds:
            diff_types[k] += 1
        if len(examples) < show:
            examples.append((item, sorted(only_m)[:6], sorted(only_t)[:6]))
    n = len(items) - skipped
    ign = f', ignoring {",".join(ignore)}' if ignore else ''
    print(f'== trees: {same}/{n} files identical node multisets (typescript-estree; {skipped} it refused{ign})')
    for k, c in diff_types.most_common(25):
        print(f'  {k:36} differs in {c} files')
    for (lang, p, label), om, ot in examples:
        print(f'  {label}\n      mep only: {om}\n      estree only: {ot}')


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--binary', default=str(BINARY))
    ap.add_argument('--node-path', default=os.environ.get('NODE_PATH', ''))
    ap.add_argument('--files', nargs='*')
    ap.add_argument('--conformance', nargs='*', help='TypeScript tests/cases dirs (conformance, compiler)')
    ap.add_argument('--no-corpus', action='store_true')
    ap.add_argument('--mutants', type=int, default=0)
    ap.add_argument('--damage', type=int, default=3)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--trees', type=int, default=0, help='compare the trees of N files both accept')
    ap.add_argument('--show', type=int, default=10)
    ap.add_argument('--ignore', default='',
                    help='also compare without these node types, comma-separated (TemplateElement: '
                    'typescript-estree ranges include the delimiters, acorn\'s do not)')
    ap.add_argument('--jobs', type=int, default=os.cpu_count() or 4)
    a = ap.parse_args()

    env = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}
    if a.node_path:
        env['NODE_PATH'] = a.node_path
    tmp = tempfile.mkdtemp(prefix='js-parse-ts-check-')
    try:
        run(a, env, tmp)
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def run(a, env, tmp):

    sets = []
    if a.files:
        sets.append(('files', [(lang_of(f) or 'ts', f, f) for f in a.files]))
    elif not a.no_corpus:
        items = []
        for c in CORPORA:
            if c.exists():
                for line in c.read_text().splitlines():
                    if line and lang_of(line):
                        items.append((lang_of(line), line, line))
        sets.append(('corpus', items))
    if a.conformance:
        udir = tempfile.mkdtemp(prefix='units-', dir=tmp)
        sets.append(('conformance', split_conformance(a.conformance, udir)))

    for title, items in sets:
        mine = errors_mep(a.binary, items, tmp, a.jobs, env)
        theirs = errors_tsc(items, tmp, a.jobs, env)
        cats = compare(items, mine, theirs)
        report(title, items, cats, a.show)
        if a.trees:
            acc = [it for it, _, _ in cats.get('accepted', [])]
            random.Random(a.seed).shuffle(acc)
            sample = acc[:a.trees]
            md, td = shapes(a.binary, sample, tmp, a.jobs, env)
            compare_trees(sample, md, td, a.show)
            if a.ignore:
                compare_trees(sample, md, td, a.show, a.ignore.split(','))
        if a.mutants:
            rng = random.Random(a.seed)
            mdir = tempfile.mkdtemp(prefix='mutants-', dir=tmp)
            damaged = []
            for lang, p, label in items:
                try:
                    text = Path(p).read_text(encoding='utf-8', errors='replace')
                except OSError:
                    continue
                for k in range(a.mutants):
                    ext = {'tsx': '.tsx', 'dts': '.d.ts', 'ts': '.ts'}[lang]
                    q = Path(mdir) / f'{len(damaged)}{ext}'
                    q.write_text(damage(text, rng, a.damage), encoding='utf-8')
                    damaged.append((lang, str(q), f'{label} (mutant {k})'))
            mine = errors_mep(a.binary, damaged, tmp, a.jobs, env)
            theirs = errors_tsc(damaged, tmp, a.jobs, env)
            report(title + ' mutants', damaged, compare(damaged, mine, theirs), a.show)


if __name__ == '__main__':
    main()
