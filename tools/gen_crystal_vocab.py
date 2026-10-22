#!/usr/bin/env python3
"""Regenerates src/crystal_lsp_stdlib.cpp from Crystal's own API docs.

mep's own Crystal language server completes, hovers and signature-helps
the standard library from this table: every documented type (with its
superclass, included and extended modules and full ancestor chain, so a
lookup can walk them), every method, constructor and macro with its
argument list and return type exactly as the official API docs print
them, and every constant, enum members included.

Generated rather than written, for the reason gen_cl_names.py gives: a
server that remembers a language's vocabulary from memory gets it wrong,
and wrong in the direction that invents diagnostics. The source is the
`index.json` that `crystal docs` writes next to the HTML API docs, run
over the stdlib's `docs_main.cr` -- the same entry point the official
API docs at crystal-lang.org/api are built from (so it includes the
fictitious `Crystal::Macros` API, which documents the macro language,
and excludes the compiler itself).

Producing index.json (what `just crystal-vocab` does):

    STD=$(crystal env CRYSTAL_PATH | tr ':' '\\n' | grep -v '^lib$' | tail -1)
    LLVM_CONFIG=true LLVM_VERSION=<llvm> LLVM_TARGETS=X86 LLVM_LDFLAGS=" " \\
        crystal docs $STD/docs_main.cr --project-name=Crystal \\
        --project-version=<version> -o /dev/shm/crystal-docs

The LLVM_* variables stand in for `llvm-config`, which docs_main.cr's
`require "./llvm"` would otherwise shell out to; `crystal docs` never
links anything, so their values only name LibLLVM::VERSION.

Then, from the repository root:

    python3 tools/gen_crystal_vocab.py /dev/shm/crystal-docs/index.json \\
        --version 1.19.1 -o src/crystal_lsp_stdlib.cpp

Decisions:

* Argument lists are the docs' own `args_string` when the JSON carries
  one. The docs generator omits it when a def has no positional
  parameters (only a block, only `**opts`, or nothing), so for those it
  is rebuilt here from the JSON `def` following
  Crystal::Doc::Method#args_to_html; the return type and `forall`
  clause then come from `def.return_type` or the `html_id` (which is the
  same rendering with spaces deleted). The " : Ret" suffix is split off
  into `ret`; " forall T" stays in `args`.
* Docs are the first paragraph of the Markdown `doc`, lines joined by
  single spaces (Markdown kept), capped at 600 bytes. A paragraph that
  is a fenced code block is skipped.
* Top-level defs (`puts`, `raise`, `spawn` ...) and macros (`record`,
  `p!` ...) have owner "". The docs list top-level defs as class
  methods of the program; they are kind 0 here because they are called
  without a receiver.
* Types are sorted by name, methods by (owner, name) with overloads in
  docs order, constants by owner with declaration order kept (enum
  members stay in value order). All sorts are byte comparisons.
"""

import argparse
import json
import re
import sys

MAX_DOC = 600


def c_str(s):
    out = ['"']
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
        elif o < 0x20 or o == 0x7f:
            out.append('\\%03o' % o)
        else:
            out.append(ch)
    out.append('"')
    return ''.join(out)


def summary(doc):
    if not doc:
        return ''
    lines = doc.replace('\r', '').split('\n')
    paras, cur, fence = [], [], False
    for ln in lines:
        st = ln.strip()
        if st.startswith('```'):
            fence = not fence
            cur.append(ln)
            continue
        if not fence and st == '':
            if cur:
                paras.append(cur)
                cur = []
            continue
        cur.append(ln)
    if cur:
        paras.append(cur)
    for p in paras:
        if p[0].strip().startswith('```'):
            continue
        text = ' '.join(x.strip() for x in p).strip()
        if len(text.encode()) > MAX_DOC:
            b = text.encode()[:MAX_DOC]
            text = b.decode('utf-8', 'ignore').rstrip() + '...'
        return text
    return ''


def strip_global(t):
    # The docs print `::Nil`, `::Tuple(...)` as `Nil`, `Tuple(...)`.
    return re.sub(r'(?<![\w:])::(?=[A-Z])', '', t)


def restriction(r):
    r = strip_global(r)
    if r.startswith('(') and r.endswith(')') and '->' in r:
        depth = 0
        for i, ch in enumerate(r):
            depth += ch == '('
            depth -= ch == ')'
            if depth == 0 and i != len(r) - 1:
                break
        else:
            r = r[1:-1]
            if r.endswith('->'):
                r += ' '
    return r


def arg_s(a, macro):
    s = ''
    ext, name = a.get('external_name', ''), a.get('name', '')
    if ext != name:
        s += (ext or '_') + ' '
    s += name
    if not macro and a.get('restriction'):
        s += ' : ' + restriction(a['restriction'])
    if 'default_value' in a:
        s += ' = ' + a['default_value']
    return s


def rebuild_args(d, macro):
    args = d.get('args') or []
    has = args or d.get('double_splat') or d.get('block_arg') or \
        (not macro and d.get('block_arity') is not None)
    if not has:
        return ''
    parts = []
    for i, a in enumerate(args):
        parts.append(('*' if d.get('splat_index') == i else '') + arg_s(a, macro))
    if d.get('double_splat'):
        parts.append('**' + arg_s(d['double_splat'], macro))
    if d.get('block_arg'):
        parts.append('&' + arg_s(d['block_arg'], macro))
    elif not macro and d.get('block_arity') is not None:
        parts.append('&')
    return '(' + ', '.join(parts) + ')'


def respace(t):
    t = t.replace('|', ' | ').replace(',', ', ').replace('->', ' -> ')
    return re.sub(r' +', ' ', t).strip()


SUFFIX = re.compile(r'-(instance-method|class-method|macro)$')


def split_sig(m, macro):
    """Returns (args, ret) for a doc method/macro entry."""
    d = m.get('def') or {}
    s = m.get('args_string')
    if s is not None:
        if macro:
            return s, ''
        # "(a : X) : Ret forall T" -> args "(a : X) forall T", ret "Ret".
        forall = ''
        fm = re.search(r' forall [^()]*$', s)
        if fm:
            forall, s = fm.group(0), s[:fm.start()]
        ret = ''
        if s.startswith('('):
            depth = 0
            for i, ch in enumerate(s):
                depth += ch == '('
                depth -= ch == ')'
                if depth == 0:
                    rest = s[i + 1:]
                    if rest.startswith(' : '):
                        ret = rest[3:]
                        s = s[:i + 1]
                    break
        return s + forall, ret
    args = rebuild_args(d, macro)
    if macro:
        return args, ''
    ident = SUFFIX.sub('', m.get('html_id', ''))
    rest = ident[len(m['name']):] if ident.startswith(m['name']) else ''
    rest = rest[len(args.replace(' ', '')):] if rest.startswith(args.replace(' ', '')) else ''
    forall = ''
    if 'forall' in rest:
        i = rest.rindex('forall')
        forall = ' forall ' + ', '.join(rest[i + 6:].split(','))
        rest = rest[:i]
    ret = ''
    if d.get('return_type'):
        ret = strip_global(d['return_type'])
    elif rest.startswith(':'):
        ret = respace(rest[1:])
    return args + forall, ret


# html_id -> the type's own full name. A type reference in the docs JSON
# (superclass, included/extended module, ancestor) gives the bare name,
# "Enumerable", while the type itself is named "Enumerable(T)", and that,
# with its type parameters, is what kTypes and the method owners use; so
# references are mapped back to the referenced type's own name, letting
# the server look an ancestor up by exact string.
CANON = {}


def index_names(t):
    if t.get('html_id'):
        CANON[t['html_id']] = t['full_name']
    for c in t.get('types') or []:
        index_names(c)


def full(ref):
    if not ref:
        return ''
    return CANON.get(ref.get('html_id'), ref['full_name'])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('index_json')
    ap.add_argument('--version', default='')
    ap.add_argument('-o', '--output')
    opts = ap.parse_args()
    with open(opts.index_json, encoding='utf-8') as f:
        data = json.load(f)
    prog = data['program']

    types, methods, consts = [], [], []

    def add_members(t, owner):
        for c in t.get('constants') or []:
            consts.append((owner, c['name'], c.get('value', ''), summary(c.get('doc'))))
        groups = [('constructors', 1), ('class_methods', 0 if owner == '' else 1),
                  ('instance_methods', 0), ('macros', 2)]
        for field, kind in groups:
            for m in t.get(field) or []:
                args, ret = split_sig(m, kind == 2)
                methods.append((owner, m['name'], args, ret, kind, summary(m.get('doc'))))

    def walk(t):
        name = t['full_name']
        sup = full(t.get('superclass'))
        if not sup and t['kind'] in ('class', 'struct', 'enum'):
            # Enums carry no `superclass` in the docs JSON; take the
            # nearest class/struct ancestor (Enum).
            sup = next((full(a) for a in t.get('ancestors') or []
                        if a.get('kind') in ('class', 'struct')), '')
        types.append((name, t['kind'], sup,
                      ','.join(full(x) for x in t.get('included_modules') or []),
                      ','.join(full(x) for x in t.get('extended_modules') or []),
                      ','.join(full(x) for x in t.get('ancestors') or []),
                      t.get('aliased', '') or '', summary(t.get('doc'))))
        add_members(t, name)
        for c in t.get('types') or []:
            walk(c)

    index_names(prog)
    add_members(prog, '')
    for t in prog.get('types') or []:
        walk(t)

    types.sort(key=lambda x: x[0].encode())
    # Python's sort is stable: overloads keep docs order.
    methods.sort(key=lambda x: (x[0].encode(), x[1].encode()))
    consts.sort(key=lambda x: x[0].encode())

    out = []
    w = out.append
    ver = (' ' + opts.version) if opts.version else ''
    w('// GENERATED by tools/gen_crystal_vocab.py from the Crystal%s API docs' % ver)
    w('// (`crystal docs` over the stdlib\'s docs_main.cr, index.json) -- do not edit.')
    w('//')
    w('// The documented standard library: types with superclass, included and')
    w('// extended modules and ancestors; methods, constructors and macros with')
    w('// argument lists and return types as the API docs print them; constants')
    w('// and enum members. Docs are first-paragraph summaries. Read by mep\'s own')
    w('// Crystal language server; see the generator\'s header for the details.')
    w('//')
    w('// %d types, %d methods, %d constants.' % (len(types), len(methods), len(consts)))
    w('')
    w('#include "crystal_lsp_stdlib.h"')
    w('')
    w('namespace {')
    w('')
    w('const CrystalStdType kTypes[] = {')
    for t in types:
        w('    {%s},' % ', '.join(c_str(x) for x in t))
    w('};')
    w('')
    w('const CrystalStdMethod kMethods[] = {')
    for m in methods:
        w('    {%s, %s, %s, %s, %d, %s},' % (c_str(m[0]), c_str(m[1]), c_str(m[2]),
                                           c_str(m[3]), m[4], c_str(m[5])))
    w('};')
    w('')
    w('const CrystalStdConst kConsts[] = {')
    for c in consts:
        w('    {%s},' % ', '.join(c_str(x) for x in c))
    w('};')
    w('')
    w('} // namespace')
    w('')
    w('const char *CrystalStdVersion() { return %s; }' % c_str(opts.version))
    w('')
    for fn, arr, ty in (('CrystalStdTypes', 'kTypes', 'CrystalStdType'),
                        ('CrystalStdMethods', 'kMethods', 'CrystalStdMethod'),
                        ('CrystalStdConsts', 'kConsts', 'CrystalStdConst')):
        w('const %s *%s(size_t *count) {' % (ty, fn))
        w('  *count = sizeof(%s) / sizeof(%s[0]);' % (arr, arr))
        w('  return %s;' % arr)
        w('}')
        w('')
    text = '\n'.join(out)
    if opts.output:
        with open(opts.output, 'w', encoding='utf-8') as f:
            f.write(text)
    else:
        sys.stdout.write(text)
    print('%d types, %d methods, %d constants' % (len(types), len(methods), len(consts)),
          file=sys.stderr)


if __name__ == '__main__':
    main()
