#!/usr/bin/env python3
"""Every Maxima tree mep's grammar builds, against Maxima's own reader.

    python3 tools/ts_grammar_diff/maxima_check.py [--show N] [--files files.txt]

The grammar (grammars/tree-sitter-maxima) and mep's Maxima reader
(src/maxima_ast.cpp -- itself checked against Maxima's mread-raw over the
whole share/ tree) parse each corpus file; maxima_check.cpp renders the
reader's statements as the node types the grammar should produce, and the
two are compared statement by statement, every leaf with its byte range.

Buckets: identical files; files the reader rejects (the grammar is only
asked not to crash there); files that declare their own operators
(`infix("~")` -- a context-free grammar cannot learn them); and
differences, grouped by the first differing node, with an example.
"""
import argparse
import collections
import hashlib
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
TOOL = Path(__file__).resolve().parent
RUNTIME = REPO / 'third_party' / 'tree-sitter'
GRAMMAR = REPO / 'grammars' / 'tree-sitter-maxima' / 'src'
OUT = REPO / 'build' / 'grammar-diff' / 'maxima' / 'check'


def build():
    sources = [GRAMMAR / 'parser.c', GRAMMAR / 'scanner.c', RUNTIME / 'src' / 'lib.c']
    cxx_sources = [TOOL / 'maxima_check.cpp', REPO / 'src' / 'maxima_ast.cpp']
    key = hashlib.sha256()
    for p in sources + cxx_sources + [REPO / 'src' / 'maxima_ast.h']:
        key.update(p.read_bytes())
    exe, stamp = OUT / 'maxima_check', OUT / 'key'
    if exe.exists() and stamp.exists() and stamp.read_text() == key.hexdigest():
        return exe
    OUT.mkdir(parents=True, exist_ok=True)
    cc, cxx = os.environ.get('CC', 'cc'), os.environ.get('CXX', 'c++')
    objs = []
    for c in sources:
        o = OUT / (c.name + '.o')
        subprocess.run([cc, '-O2', '-w', '-I', str(GRAMMAR), '-I', str(RUNTIME / 'include'), '-I', str(RUNTIME / 'src'),
                        '-c', str(c), '-o', str(o)], check=True)
        objs.append(str(o))
    for c in cxx_sources:
        o = OUT / (c.name + '.o')
        subprocess.run([cxx, '-O2', '-std=gnu++20', '-w', '-I', str(REPO / 'src'), '-I', str(RUNTIME / 'include'),
                        '-c', str(c), '-o', str(o)], check=True)
        objs.append(str(o))
    subprocess.run([cxx, *objs, '-o', str(exe)], check=True)
    stamp.write_text(key.hexdigest())
    return exe


def first_diff(a, b):
    """The innermost node where two s-expressions part, as 'reader -> grammar'."""
    i = 0
    while i < min(len(a), len(b)) and a[i] == b[i]:
        i += 1
    def node_at(s, i):
        depth, j = 0, i
        while j > 0:
            j -= 1
            if s[j] == ')':
                depth += 1
            elif s[j] == '(':
                if depth == 0:
                    break
                depth -= 1
        k = j + 1
        while k < len(s) and s[k] not in ' )':
            k += 1
        return s[j + 1:k]
    def token_at(s, i):
        k = i
        while k < len(s) and s[k] not in ' )':
            k += 1
        return s[i:k] or s[i:i + 1]
    return f'{node_at(a, i)}:{token_at(a, i)} -> {node_at(b, i)}:{token_at(b, i)}'


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--files', default=str(REPO / 'build' / 'grammar-corpus' / 'maxima' / 'files.txt'))
    ap.add_argument('--show', type=int, default=2, help='examples per difference kind')
    a = ap.parse_args()
    exe = build()
    files = [l for l in Path(a.files).read_text().splitlines() if l]
    p = subprocess.run([str(exe)], input='\n'.join(files) + '\n', capture_output=True, text=True,
                       errors='replace')
    if p.returncode != 0:
        print(p.stderr, file=sys.stderr)
        sys.exit(f'maxima_check crashed (exit {p.returncode})')
    stats = collections.Counter()
    kinds = collections.defaultdict(list)
    cur = None
    r_line = None
    differs = False
    userops = False
    for line in p.stdout.splitlines():
        if line.startswith('@@FILE '):
            cur, differs, userops = line[7:], False, False
        elif line.startswith('@@USEROPS'):
            userops = True
        elif line.startswith('@@REJECT'):
            stats['rejected by the reader'] += 1
            cur = None
        elif line.startswith('R '):
            r_line = line[2:]
        elif line.startswith('T ') and cur:
            t = line[2:]
            if t != r_line and not differs:
                differs = True
                key = 'declares its own operators' if userops else first_diff(r_line, t)
                kinds[key].append((cur, r_line, t))
        elif line.startswith('@@COUNT') and cur:
            stats['differ' if differs else 'identical'] += 1
    total = sum(stats.values())
    print(f'{total} files: ' + ', '.join(f'{v} {k}' for k, v in stats.most_common()))
    for k, ex in sorted(kinds.items(), key=lambda kv: -len(kv[1])):
        print(f'  {len(ex):5}  {k}')
        for path, r, t in ex[:a.show]:
            print(f'         {path}')
            print(f'           reader : {r[:300]}')
            print(f'           grammar: {t[:300]}')
    sys.exit(1 if stats['differ'] else 0)


if __name__ == '__main__':
    main()
