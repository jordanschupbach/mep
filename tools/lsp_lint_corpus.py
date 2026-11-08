#!/usr/bin/env python3
"""Run one of mep's language servers as a linter over a corpus and tally
what it reports -- the false-positive calibration step every in-house LSP
goes through (plans/LANGUAGE_PARITY_PLAN.md, section 2).

    just lsp-lint-corpus nix                     # counts per code
    just lsp-lint-corpus nix --code unused-let   # every occurrence of one code, with its line
    just lsp-lint-corpus nix --sample 2000 --examples 5

Reads `build/native/mep-<lang>-lsp --lint FILE...` output, whose lines
are `path:line:col: severity: message [code]` (every mep server prints
that shape). Files go to the server in batches; a batch that crashes (a
signal, not exit 1) is re-run file by file so the report names the file.

On a corpus of code that is known to work, every error is a false
positive until shown otherwise, and a warning that fires on a large share
of files is probably a style opinion rather than a bug finder: the table
sorts by how many files a code fires in to make those stand out.
"""

import argparse
import collections
import concurrent.futures
import json
import os
import random
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools' / 'ts_grammar_diff'))
import grammar_diff  # noqa: E402

# Language -> server binary, where the name differs from mep-<lang>-lsp.
BINARIES = {'common_lisp': 'mep-lisp-lsp', 'commonlisp': 'mep-lisp-lsp', 'c_sharp': 'mep-csharp-lsp',
            'javascript': 'mep-js-lsp'}

LINE = re.compile(r'^(.*):(\d+):(\d+): (error|warning|info|hint): (.*) \[([^\]]+)\]$')


def lint(binary, files, timeout):
    """(diagnostics, crashed files, timed-out files)."""
    try:
        p = subprocess.run([binary, '--lint', *files], capture_output=True, timeout=timeout)
    except subprocess.TimeoutExpired:
        if len(files) == 1:
            return [], [], files
        out = ([], [], [])
        for f in files:
            d, c, t = lint(binary, [f], timeout)
            out[0].extend(d), out[1].extend(c), out[2].extend(t)
        return out
    if p.returncode < 0:
        if len(files) == 1:
            return [], files, []
        out = ([], [], [])
        for f in files:
            d, c, t = lint(binary, [f], timeout)
            out[0].extend(d), out[1].extend(c), out[2].extend(t)
        return out
    diags = []
    for line in p.stdout.decode('utf-8', 'replace').splitlines():
        m = LINE.match(line)
        if m:
            diags.append(dict(path=m[1], line=int(m[2]), col=int(m[3]), severity=m[4], message=m[5], code=m[6]))
    return diags, [], []


def source_line(path, line):
    try:
        with open(path, 'rb') as f:
            for k, text in enumerate(f, 1):
                if k == line:
                    return text.decode('utf-8', 'replace').rstrip('\n')
    except OSError:
        pass
    return ''


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n\n')[0])
    ap.add_argument('lang')
    ap.add_argument('--corpus', help='a files.txt (default: build/grammar-corpus/<lang>/files.txt)')
    ap.add_argument('--binary', help='the server (default: build/native/mep-<lang>-lsp)')
    ap.add_argument('--sample', type=int)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--batch', type=int, default=200)
    ap.add_argument('--jobs', type=int, default=os.cpu_count())
    ap.add_argument('--timeout', type=float, default=300)
    ap.add_argument('--code', action='append', help='list every occurrence of this code (repeatable)')
    ap.add_argument('--examples', type=int, default=2, help='example occurrences per code in the table')
    ap.add_argument('--json', help='write every diagnostic here')
    a = ap.parse_args()

    binary = Path(a.binary) if a.binary else REPO / 'build' / 'native' / BINARIES.get(a.lang, f'mep-{a.lang}-lsp')
    if not binary.exists():
        sys.exit(f'lsp_lint_corpus: {binary} not built')
    corpus = Path(a.corpus) if a.corpus else grammar_diff.corpus_list(a.lang)
    if not corpus.exists():
        sys.exit(f'lsp_lint_corpus: no corpus at {corpus}; run: just grammar-corpus {a.lang}')
    files = [f for f in corpus.read_text().split('\n') if f]
    if a.sample and a.sample < len(files):
        files = sorted(random.Random(a.seed).sample(files, a.sample))

    diags, crashed, timed_out = [], [], []
    batches = [files[i:i + a.batch] for i in range(0, len(files), a.batch)]
    with concurrent.futures.ThreadPoolExecutor(a.jobs) as ex:
        for d, c, t in ex.map(lambda b: lint(str(binary), b, a.timeout), batches):
            diags += d
            crashed += c
            timed_out += t

    by_code = collections.defaultdict(list)
    for d in diags:
        by_code[d['code']].append(d)
    files_with_any = len({d['path'] for d in diags})
    print(f'{a.lang}: {len(files)} files, {len(diags)} diagnostics in {files_with_any} files')
    print(f'{"code":32} {"sev":8} {"count":>7} {"files":>7} {"% files":>8}')
    rows = sorted(by_code.items(), key=lambda kv: -len({d['path'] for d in kv[1]}))
    for code, ds in rows:
        nfiles = len({d['path'] for d in ds})
        sev = collections.Counter(d['severity'] for d in ds).most_common(1)[0][0]
        print(f'{code:32} {sev:8} {len(ds):7} {nfiles:7} {100 * nfiles / len(files):7.2f}%')
        for d in ds[:a.examples]:
            print(f'    {d["path"]}:{d["line"]}:{d["col"]}: {d["message"]}')
            src = source_line(d['path'], d['line']).strip()
            if src:
                print(f'        | {src[:110]}')
    for f in crashed:
        print(f'CRASH: {f}')
    for f in timed_out:
        print(f'TIMEOUT: {f}')

    for code in a.code or []:
        print(f'\n=== {code}: {len(by_code.get(code, []))}')
        for d in by_code.get(code, []):
            print(f'{d["path"]}:{d["line"]}:{d["col"]}: {d["message"]}')
            print(f'    | {source_line(d["path"], d["line"]).strip()[:140]}')
    if a.json:
        Path(a.json).write_text(json.dumps(diags, indent=1))
    return 1 if crashed or timed_out else 0


if __name__ == '__main__':
    sys.exit(main())
