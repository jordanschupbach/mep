#!/usr/bin/env python3
"""Generate mep-zig-lsp's vocabulary (src/zig_lsp_vocab_data.cpp) from Zig's
own source tree.

    python3 -I tools/gen_zig_vocab.py <zig source tree> > src/zig_lsp_vocab_data.cpp
    just zig-lsp-vocab        # the same, with nixpkgs' zig.src

Two tables:

  builtins   every `@name` in doc/langref.html.in: its signature line and
             the first paragraphs of its description.
  std        the public declarations of lib/std: each file's top-level
             `pub fn`/`pub const`/`pub var` (std/mem.zig -> std.mem.eql), and
             one level into a public container (`pub const Foo = struct {`,
             a file-struct such as std/Io/Writer.zig's own fields' siblings):
             the name, a one-line signature and the `///` doc comment.

Zig's standard library and language reference are MIT licensed (Copyright
(c) Zig contributors), so the doc text is embedded, with that notice in the
generated file. Nothing is evaluated: this is a text scan of declarations
that start a line, which is how the std is written (`zig fmt` puts every
declaration at the start of its line).
"""

import html
import os
import re
import sys

MAX_DOC = 900


def c_string(s):
    out = ['"']
    for ch in s:
        o = ord(ch)
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n')
        elif ch == '?':
            out.append('\\?')  # never a trigraph
        elif o < 0x20:
            out.append('\\x%02x""' % o)
        else:
            out.append(ch)
    out.append('"')
    return ''.join(out)


def clip(text, n=MAX_DOC):
    text = text.strip()
    if len(text) <= n:
        return text
    cut = text.rfind('\n', 0, n)
    if cut < n // 2:
        cut = text.rfind(' ', 0, n)
    return text[:cut].rstrip() + ' ...'


def collapse(s):
    return re.sub(r'\s+', ' ', s).strip()


# ---- builtins from the language reference ----------------------------------

def builtins(root):
    src = open(os.path.join(root, 'doc', 'langref.html.in'), encoding='utf-8').read()
    rows = {}
    for m in re.finditer(r'\{#header_open\|(@\w+)#\}(.*?)\{#header_close#\}', src, re.S):
        name, body = m.group(1), m.group(2)
        sig = re.search(r'\{#syntax#\}(.*?)\{#endsyntax#\}', body, re.S)
        if not sig:
            continue
        signature = collapse(html.unescape(sig.group(1)))
        paras = []
        for p in re.findall(r'<p>(.*?)</p>', body, re.S):
            p = re.sub(r'\{#syntax#\}(.*?)\{#endsyntax#\}', r'`\1`', p, flags=re.S)
            p = re.sub(r'\{#link\|([^|#]*)(\|[^#]*)?#\}', r'\1', p)
            p = re.sub(r'<[^>]+>', '', p)
            p = collapse(html.unescape(p))
            if p:
                paras.append(p)
            if sum(len(x) for x in paras) > MAX_DOC:
                break
        rows[name] = ('builtin', signature, clip('\n\n'.join(paras)))
    return rows


# ---- BuiltinFn.zig ------------------------------------------------------------

def builtin_fns(root):
    """lib/std/zig/BuiltinFn.zig's list: name -> (param_count or -1,
    illegal_outside_function, allows_lvalue)."""
    src = open(os.path.join(root, 'lib', 'std', 'zig', 'BuiltinFn.zig'), encoding='utf-8').read()
    out = {}
    for m in re.finditer(r'\.\{\s*"(@\w+)",\s*\.\{(.*?)\n\s*\},\s*\n\s*\}', src, re.S):
        name, body = m.group(1), m.group(2)
        pc = re.search(r'\.param_count\s*=\s*(null|\d+)', body)
        count = -1 if pc is None or pc.group(1) == 'null' else int(pc.group(1))
        out[name] = (count, '.illegal_outside_function = true' in body, '.allows_lvalue = true' in body)
    if len(out) < 100:
        sys.exit('BuiltinFn.zig: only %d builtins found' % len(out))
    return out


# ---- the standard library ---------------------------------------------------

DECL = re.compile(r'^(?P<indent> *)pub\s+(?:(?:inline|noinline|extern(?:\s+"[^"]*")?|export|threadlocal)\s+)*'
                  r'(?P<kw>fn|const|var)\s+(?P<name>@"[^"]*"|[A-Za-z_]\w*)')
CONTAINER = re.compile(r'=\s*(?:extern\s+|packed\s+)?(?:struct|union|enum|opaque)\b[^{]*\{\s*$')


def signature(lines, i, kw):
    """The declaration starting at lines[i] as one line: a function up to its
    body, anything else up to the end of its first line."""
    if kw != 'fn':
        line = lines[i].strip()
        return collapse(line[len('pub '):] if line.startswith('pub ') else line)[:160]
    text, depth = '', 0
    for j in range(i, min(i + 40, len(lines))):
        line = re.sub(r'//.*$', '', lines[j])
        for k, ch in enumerate(line):
            if ch == '(':
                depth += 1
            elif ch == ')':
                depth -= 1
            elif depth == 0 and ch in '{;' and 'fn' in text + line[:k]:
                return collapse(text + line[:k]).removeprefix('pub ')
        text += line + ' '
    return collapse(text).removeprefix('pub ')[:300]


def doc_above(lines, i):
    doc = []
    j = i - 1
    while j >= 0 and lines[j].lstrip().startswith('///'):
        doc.append(lines[j].lstrip()[3:].removeprefix(' '))
        j -= 1
    return clip('\n'.join(reversed(doc)))


def module_name(rel):
    parts = rel[:-len('.zig')].split('/')
    if parts == ['std', 'std']:
        return 'std'
    return '.'.join(parts)


def std_rows(root):
    std = os.path.join(root, 'lib')
    rows = {}
    for dirpath, dirnames, filenames in os.walk(os.path.join(std, 'std')):
        dirnames.sort()
        for fn in sorted(filenames):
            if not fn.endswith('.zig') or fn.endswith('_test.zig') or fn == 'test.zig':
                continue
            path = os.path.join(dirpath, fn)
            rel = os.path.relpath(path, std).replace(os.sep, '/')
            if '/testdata/' in rel or rel.startswith('std/zig/c_translation'):
                continue
            mod = module_name(rel)
            lines = open(path, encoding='utf-8', errors='replace').read().split('\n')
            container = None  # (name, indent of its members)
            for i, line in enumerate(lines):
                m = DECL.match(line)
                if line.strip() == '};' and container and len(line) - len(line.lstrip()) == 0:
                    container = None
                if not m:
                    continue
                indent = len(m.group('indent'))
                name = m.group('name')
                kw = m.group('kw')
                if indent == 0:
                    qual = f'{mod}.{name}'
                    container = (qual, 4) if kw == 'const' and CONTAINER.search(line) else None
                elif container and indent == container[1]:
                    qual = f'{container[0]}.{name}'
                else:
                    continue
                kind = {'fn': 'fn', 'var': 'var'}[kw] if kw != 'const' else (
                    'type' if CONTAINER.search(line) or re.search(r'=\s*(?:error\s*\{|@Type|@This\(\))', line)
                    else 'const')
                if qual not in rows:
                    rows[qual] = (kind, signature(lines, i, kw), doc_above(lines, i))
    return rows


LOOKUPS = """template <typename T>
static const T *findSorted(const T *table, std::size_t count, std::string_view name) {
    std::size_t lo = 0, hi = count;
    while (lo < hi) {
        std::size_t mid = lo + (hi - lo) / 2;
        std::string_view n = table[mid].name;
        if (n == name) return &table[mid];
        if (n < name)
            lo = mid + 1;
        else
            hi = mid;
    }
    return nullptr;
}

const BuiltinInfo *findBuiltin(std::string_view name) { return findSorted(kBuiltins, kBuiltinCount, name); }

const VocabEntry *findVocab(std::string_view name) { return findSorted(kVocab, kVocabCount, name); }
"""


def main():
    if len(sys.argv) != 2:
        sys.exit(__doc__)
    root = sys.argv[1]
    b = builtins(root)
    s = std_rows(root)
    rows = sorted(list(b.items()) + list(s.items()), key=lambda r: r[0].encode())
    print('// Generated by tools/gen_zig_vocab.py from Zig\'s own source tree -- do not edit.')
    print('// `just zig-lsp-vocab` regenerates it.')
    print('//')
    print('// The signatures and doc text below are taken from Zig\'s doc/langref.html.in')
    print('// and lib/std, under the MIT License (Expat):')
    print('//')
    for line in open(os.path.join(root, 'LICENSE'), encoding='utf-8').read().rstrip().split('\n'):
        print('//' + (' ' + line if line else ''))
    print()
    print('#include "zig_lsp_vocab.h"')
    print()
    print('namespace zig_lsp {')
    print()
    print('// Sorted by name (byte order), for binary search.')
    print('const VocabEntry kVocab[] = {')
    for name, (kind, sig, doc) in rows:
        print(f'    {{{c_string(name)}, {c_string(kind)}, {c_string(sig)}, {c_string(doc)}}},')
    print('};')
    print()
    print('const std::size_t kVocabCount = sizeof(kVocab) / sizeof(kVocab[0]);')
    print()
    fns = builtin_fns(root)
    print('// lib/std/zig/BuiltinFn.zig, sorted by name.')
    print('const BuiltinInfo kBuiltins[] = {')
    for name in sorted(fns):
        count, illegal, lvalue = fns[name]
        print(f'    {{{c_string(name)}, {count}, {str(illegal).lower()}, {str(lvalue).lower()}}},')
    print('};')
    print()
    print('const std::size_t kBuiltinCount = sizeof(kBuiltins) / sizeof(kBuiltins[0]);')
    print()
    print(LOOKUPS)
    print('}  // namespace zig_lsp')
    print(f'// {len(b)} builtins, {len(s)} std declarations, {len(fns)} BuiltinFn entries.', file=sys.stderr)


if __name__ == '__main__':
    main()
