#!/usr/bin/env python3
"""Generate src/tcl_lsp_vocab.cpp: the names mep-tcl-lsp knows.

    just tcl-lsp-vocab        (= python3 tools/gen_tcl_vocab.py TCL_SOURCE_DIR)

Two sources, both Tcl's own:

  - tclsh (the one on PATH) lists its built-in commands, their
    subcommands, expr's functions and the variables it starts with, and
    says how many arguments each command takes: tools/tcl_vocab_probe.tcl
    calls every one in a child interpreter with 0..30 and 40 arguments and
    keeps the counts that do not end in "wrong # args" -- and Tcl's usage
    line from that error.
  - Tcl's manual pages (doc/*.n in the Tcl source) give each command's
    synopsis and the first part of its description, and each subcommand's
    entry. Tcl and its documentation are under Tcl's BSD-style license
    (license.terms), which asks that the notice go with copies: it is
    reproduced at the top of the generated file.

A command whose usage has options (`?-option ...?`) takes any count from its
minimum up: the probe's `x` arguments are never options, so it cannot see
how many words options add.
"""

import argparse
import re
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / 'src' / 'tcl_lsp_vocab.cpp'
PROBE = REPO / 'tools' / 'tcl_vocab_probe.tcl'

# Variables tclsh sets for a script (tclvars.n) that `info globals` in a
# fresh interpreter does not show.
EXTRA_VARS = ['argc', 'argv', 'argv0', 'tcl_precision', 'tcl_rcFileName', 'tcl_traceCompile', 'tcl_traceExec',
              'tcl_wordchars', 'tcl_nonwordchars', 'auto_index', 'auto_noexec', 'auto_noload', 'tcl_interactive']


def cstr(s):
    out = []
    for ch in s:
        o = ord(ch)
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '\t':
            out.append('\\t')
        elif ch == '?':
            out.append('\\?')  # never a trigraph
        elif o < 0x20:
            out.append('\\x%02x' % o)
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


# --- nroff -> markdown -------------------------------------------------------------

ESCAPES = {r'\-': '-', r'\e': '\\', r'\&': '', r'\(em': '—', r'\(en': '–', r'\(bu': '•', r'\(co': '©',
           r'\(+-': '±', r'\(mu': '×', r'\(<=': '≤', r'\(>=': '≥', r'\(!=': '≠', r'\(->': '→', r'\(*a': 'α',
           r'\(*b': 'β', r'\(*p': 'π', r'\(aa': "'", r'\(ga': '`', r'\|': '', r'\^': '', r'\0': ' ',
           r'\ ': ' ', r'\%': '', r'\(dq': '"', r'\(oq': "'", r'\(cq': "'", r'\(lq': '"', r'\(rq': '"',
           r'\(ti': '~', r'\(ha': '^', r'\(rs': '\\', r'\(fm': "'"}


def inline(text, code=False):
    """One line of nroff text: fonts become markdown, escapes become characters."""
    out = []
    font = 'R'
    i = 0
    buf = []

    def flush():
        s = ''.join(buf)
        buf.clear()
        if not s:
            return
        if code or font == 'R' or not s.strip():
            out.append(s)
        elif font == 'B':
            out.append('**' + s + '**')
        else:
            out.append('*' + s + '*')

    while i < len(text):
        if text.startswith('\\f', i) and i + 2 < len(text):
            flush()
            f = text[i + 2]
            if f == '(':
                f = text[i + 3:i + 5]
                i += 5
            else:
                i += 3
            font = {'B': 'B', 'I': 'I', 'R': 'R', 'P': 'R', 'CW': 'B', 'CB': 'B'}.get(f, 'R')
            continue
        for k, v in ESCAPES.items():
            if text.startswith(k, i):
                buf.append(v)
                i += len(k)
                break
        else:
            if text.startswith('\\*(', i):
                i += 5
            elif text[i] == '\\' and i + 1 < len(text):
                buf.append(text[i + 1])
                i += 2
            else:
                buf.append(text[i])
                i += 1
    flush()
    s = ''.join(out)
    s = s.replace('****', '').replace('**  **', ' ')
    return s


def plain(text):
    return re.sub(r'\*+', '', inline(text))


def parse_man(path):
    """[(section, [items])] where an item is ('p', text) | ('tp', tag, [paragraphs]) | ('code', text)."""
    lines = path.read_text(encoding='utf-8', errors='replace').split('\n')
    sections = []
    cur = None
    para = []
    code = None
    tp = None  # (tag, [paragraph texts]) collecting
    pending_tag = False

    def end_para():
        nonlocal para
        if para and cur is not None:
            text = ' '.join(para).strip()
            if text:
                if tp is not None:
                    tp[1].append(text)
                else:
                    cur[1].append(('p', text))
        para = []

    def end_tp():
        nonlocal tp
        end_para()
        if tp is not None and cur is not None:
            cur[1].append(('tp', tp[0], tp[1]))
        tp = None

    for raw in lines:
        if raw.startswith(".\\\"") or raw.startswith("'\\\""):
            continue
        if code is not None:
            if raw.startswith('.CE'):
                if cur is not None:
                    item = ('code', '\n'.join(code))
                    if tp is not None:
                        tp[1].append('```tcl\n' + item[1] + '\n```')
                    else:
                        cur[1].append(item)
                code = None
            elif not raw.startswith('.'):
                code.append(plain(raw))
            continue
        if pending_tag and not raw.startswith('.'):
            tp = (inline(raw), [])
            pending_tag = False
            continue
        if raw.startswith('.'):
            m = re.match(r'^\.(\w+)\s*(.*)$', raw)
            if not m:
                continue
            req, arg = m.group(1), m.group(2)
            if req == 'SH':
                end_tp()
                cur = (arg.strip('"').strip(), [])
                sections.append(cur)
            elif req in ('PP', 'LP', 'P', 'sp', 'br'):
                end_para()
                if req in ('PP', 'LP', 'P') and tp is not None and False:
                    end_tp()
            elif req == 'TP':
                end_tp()
                pending_tag = True
            elif req == 'IP':
                end_para()
            elif req == 'CS':
                end_para()
                code = []
            elif req in ('RS', 'RE'):
                end_para()
            elif req in ('QW', 'QR'):
                para.append('"' + inline(arg.strip('"')) + '"')
            elif req == 'PQ':
                para.append('(' + inline(arg.strip('"')) + ')')
            elif req == 'UL':
                para.append(inline(arg))
            # .VS .VE .BS .BE .so .TH .AP .DS .DE .ta ... ignored
            continue
        para.append(inline(raw))
    end_tp()
    return sections


def section(sections, name):
    for s, items in sections:
        if s.upper() == name:
            return items
    return []


def clip(paras, limit=1400):
    out = []
    n = 0
    for p in paras:
        if n and n + len(p) > limit:
            break
        out.append(p)
        n += len(p)
    return '\n\n'.join(out)


def item_text(item):
    if item[0] == 'p':
        return item[1]
    if item[0] == 'code':
        return '```tcl\n' + item[1] + '\n```'
    return '**' + item[1] + '** ' + ' '.join(item[2])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('tcl_source', help='an unpacked Tcl 8.6 source tree (doc/*.n, license.terms)')
    ap.add_argument('--probe', help='use this tcl_vocab_probe.tcl output instead of running it')
    a = ap.parse_args()
    src = Path(a.tcl_source)
    if a.probe:
        probe = Path(a.probe).read_text()
    else:
        with tempfile.TemporaryDirectory() as d:
            probe = subprocess.run(['tclsh', str(PROBE)], cwd=d, capture_output=True, text=True,
                                   check=True).stdout
    version = subprocess.run(['tclsh'], input='puts [info patchlevel]', capture_output=True,
                             text=True).stdout.strip() or '8.6'
    license_text = (src / 'license.terms').read_text()

    cmds = {}
    subs = {}
    funcs = {}
    vars_ = []
    for line in probe.splitlines():
        f = line.split('\t')
        if f[0] == 'cmd':
            cmds[f[1]] = dict(kind=f[2], mask=int(f[3]), more=f[4] == '1', usage=f[5] if len(f) > 5 else '')
        elif f[0] == 'sub':
            subs[(f[1], f[2])] = dict(mask=int(f[3]), more=f[4] == '1', usage=f[5] if len(f) > 5 else '')
        elif f[0] == 'func':
            funcs[f[1]] = int(f[2])
        elif f[0] == 'var':
            vars_.append(f[1])
    for v in EXTRA_VARS:
        if v not in vars_:
            vars_.append(v)

    # Manual pages.
    docs = {}      # command -> (synopsis lines, description)
    subdocs = {}   # (command, sub) -> (usage, doc)
    funcdocs = {}
    vardocs = {}
    for man in sorted((src / 'doc').glob('*.n')):
        sections = parse_man(man)
        names = section(sections, 'NAME')
        namelist = []
        if names:
            first = plain(names[0][1]) if names[0][0] == 'p' else ''
            namelist = [n.strip() for n in first.split(' - ')[0].split(',')]
        synopsis = [plain(it[1]) for it in section(sections, 'SYNOPSIS') if it[0] == 'p']
        syn_lines = []
        for it in section(sections, 'SYNOPSIS'):
            if it[0] == 'p':
                syn_lines.append(plain(it[1]))
        desc = section(sections, 'DESCRIPTION')
        intro = []
        for it in desc:
            if it[0] == 'tp':
                break
            intro.append(item_text(it))
        for n in namelist:
            n = n.strip()
            if n and n not in docs:
                docs[n] = (synopsis, clip(intro))
        # Subcommand entries: `.TP` whose tag starts with "\fBcmd sub".
        for _, items in sections:
            for it in items:
                if it[0] != 'tp':
                    continue
                tag = plain(it[1]).strip()
                words = tag.split()
                if len(words) >= 2 and words[0] in cmds and cmds[words[0]]['kind'] == 'ensemble':
                    key = (words[0], words[1])
                    if key not in subdocs:
                        subdocs[key] = (tag, clip(it[2], 1000))
                if man.name == 'mathfunc.n' and words:
                    fn = words[0]
                    if fn in funcs and fn not in funcdocs:
                        funcdocs[fn] = (tag, clip(it[2], 800))
                if man.name == 'tclvars.n' and words and words[0] not in vardocs:
                    vardocs[words[0]] = clip(it[2], 800)

    def arity(info):
        mask, more, usage = info['mask'], info['more'], info['usage']
        if mask < 0:
            return 0, False
        if '?-' in usage:
            low = next((n for n in range(31) if mask >> n & 1), None)
            if low is None:
                return 0, False
            return (0x7FFFFFFF >> low) << low, True
        return mask, more

    rows = []
    for name in sorted(cmds):
        info = cmds[name]
        mask, more = arity(info)
        syn, desc = docs.get(name, ([], ''))
        usage = info['usage'] or (syn[0] if syn else name)
        doc = desc
        if syn and len(syn) > 1:
            doc = '\n'.join('    ' + s for s in syn) + ('\n\n' + desc if desc else '')
        rows.append((name, info['kind'], usage, mask, more, doc))
    for (c, s) in sorted(subs):
        info = subs[(c, s)]
        mask, more = arity(info)
        tag, doc = subdocs.get((c, s), ('', ''))
        usage = info['usage'] or tag or f'{c} {s}'
        rows.append((f'{c} {s}', 'subcommand', usage, mask, more, doc))
    for fn in sorted(funcs):
        tag, doc = funcdocs.get(fn, ('', ''))
        mask = funcs[fn]
        more = fn in ('max', 'min')
        if more:
            mask = 0x7FFFFFFE
        rows.append((fn, 'function', tag or f'{fn}(...)', mask, more, doc))
    for v in sorted(vars_):
        rows.append((v, 'variable', v, 0, False, vardocs.get(v, '')))

    out = []
    out.append('// Generated by tools/gen_tcl_vocab.py -- do not edit.')
    out.append(f'// Tcl {version}: commands, argument counts and usage lines from tclsh itself, documentation')
    out.append('// from Tcl\'s manual pages (doc/*.n), which come with this notice (license.terms):')
    out.append('//')
    for l in license_text.rstrip().split('\n'):
        out.append(('// ' + l).rstrip())
    out.append('#include "tcl_lsp_vocab.h"')
    out.append('')
    out.append('namespace {')
    out.append('')
    out.append('const TclLspApi kApi[] = {')
    for name, kind, usage, mask, more, doc in rows:
        out.append(f'    {{{cstr(name)}, {cstr(kind)}, {cstr(usage)}, {mask}u, {"true" if more else "false"}, '
                   f'{cstr(doc)}}},')
    out.append('};')
    out.append('')
    out.append('}  // namespace')
    out.append('')
    out.append('const TclLspApi *TclLspApiTable(size_t *count) {')
    out.append('    *count = sizeof(kApi) / sizeof(kApi[0]);')
    out.append('    return kApi;')
    out.append('}')
    OUT.write_text('\n'.join(out) + '\n')
    print(f'{OUT}: {len(rows)} names ({len(cmds)} commands, {len(subs)} subcommands, {len(funcs)} functions, '
          f'{len(vars_)} variables)', file=sys.stderr)


if __name__ == '__main__':
    main()
