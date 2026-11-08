#!/usr/bin/env python3
"""Generate src/lua_lsp_vocab.cpp: the names mep-lua-lsp knows.

    just lua-lsp-vocab        (= python3 tools/gen_lua_vocab.py MANUAL.html)

Three sources:

  - Lua 5.4's standard library: the names and kinds from the `lua` on PATH
    (it enumerates _G and every library table), the signatures and the
    documentation from the reference manual's HTML (doc/manual.html in the
    Lua source; Lua and its manual are MIT-licensed like mep, so the text
    is embedded, with the notice).
  - mep's own API: every `mep.*` binding in src/lua_env.cpp's kMepFuncs
    table, with the signature and doc comment written above its C function,
    and every `function mep.x(...)` / `mep.x = function(...)` defined in the
    builtin Lua chunks of src/main.cpp, with the comment above it.
  - The names other Lua versions have that 5.4 does not (unpack, setfenv,
    loadstring, ...), so the server can say what happened to them.
"""

import html
import json
import re
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
OUT = REPO / 'src' / 'lua_lsp_vocab.cpp'

ENUM = r'''
local out = {}
local function kind(v) local t = type(v); return t end
for k, v in pairs(_G) do
  if type(k) == 'string' then
    out[#out + 1] = k .. '\t' .. kind(v)
    if type(v) == 'table' and k ~= '_G' and k ~= 'package' or k == 'package' then
      for kk, vv in pairs(v) do
        if type(kk) == 'string' and k ~= '_G' then out[#out + 1] = k .. '.' .. kk .. '\t' .. kind(vv) end
      end
    end
  end
end
table.sort(out)
io.write(table.concat(out, '\n'), '\n')
'''

# Names from 5.1 / 5.2 / 5.3 / LuaJIT that 5.4 does not have, and what replaced them.
OTHER_VERSIONS = {
    'unpack': 'Lua 5.1: use `table.unpack`',
    'setfenv': 'Lua 5.1: use `_ENV` (a local or upvalue named `_ENV`)',
    'getfenv': 'Lua 5.1: use `_ENV`',
    'loadstring': 'Lua 5.1: `load` takes a string',
    'module': 'Lua 5.1: return a table from the module instead',
    'math.pow': 'Lua 5.1: use `x ^ y`',
    'math.ldexp': 'Lua 5.2: use `x * 2.0 ^ e`',
    'math.frexp': 'Lua 5.2: removed',
    'math.cosh': 'Lua 5.2: removed', 'math.sinh': 'Lua 5.2: removed', 'math.tanh': 'Lua 5.2: removed',
    'math.log10': 'Lua 5.1: use `math.log(x, 10)`',
    'math.mod': 'Lua 5.0: use `math.fmod` or `%`',
    'table.getn': 'Lua 5.0: use `#t`', 'table.setn': 'Lua 5.0: removed',
    'table.maxn': 'Lua 5.1: removed', 'table.foreach': 'Lua 5.0: use a `for` loop',
    'table.foreachi': 'Lua 5.0: use a `for` loop',
    'string.gfind': 'Lua 5.0: use `string.gmatch`',
    'bit32': 'Lua 5.2: use the bitwise operators (`&`, `|`, `~`, `<<`, `>>`)',
    'bit': 'LuaJIT: use the bitwise operators (`&`, `|`, `~`, `<<`, `>>`)',
    'jit': 'LuaJIT only', 'ffi': 'LuaJIT only (`require("ffi")`)',
    'gcinfo': 'Lua 5.0: use `collectgarbage("count")`',
    'newproxy': 'Lua 5.1: use `setmetatable({}, mt)`',
    'debug.setcstacklimit': 'Lua 5.4.0-5.4.2 only',
}


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
        elif ch == '?':
            out.append('\\?')  # no trigraphs
        elif o < 32:
            out.append('\\%03o' % o)
        else:
            out.append(ch)
    return '"' + ''.join(out) + '"'


def html_to_md(h):
    h = re.sub(r'<pre>(.*?)</pre>', lambda m: '\n```lua\n' + m.group(1).strip('\n') + '\n```\n', h, flags=re.S)
    h = re.sub(r'<code>(.*?)</code>', lambda m: '`' + m.group(1) + '`', h, flags=re.S)
    h = re.sub(r'<em>(.*?)</em>', r'*\1*', h, flags=re.S)
    h = re.sub(r'<b>(.*?)</b>', r'**\1**', h, flags=re.S)
    h = re.sub(r'<li>', '\n- ', h)
    h = re.sub(r'<p>', '\n\n', h)
    h = re.sub(r'<[^>]+>', '', h)
    h = h.replace('&middot;&middot;&middot;', '...')
    h = html.unescape(h).replace(' ', ' ')
    # Collapse the manual's hard-wrapped lines into paragraphs (but not inside code blocks).
    parts = re.split(r'(```lua\n.*?\n```)', h, flags=re.S)
    out = []
    for p in parts:
        if p.startswith('```'):
            out.append('\n' + p + '\n')
        else:
            paras = [re.sub(r'\s*\n\s*', ' ', x).strip() for x in re.split(r'\n\s*\n', p)]
            out.append('\n\n'.join(x for x in paras if x))
    text = re.sub(r'\n{3,}', '\n\n', '\n\n'.join(x.strip('\n') for x in out if x.strip())).strip()
    return text


def manual_entries(path):
    doc = Path(path).read_text(encoding='latin-1')
    entries = {}
    for m in re.finditer(r'<h3><a name="pdf-([^"]+)"><code>(.*?)</code></a></h3>(.*?)(?=<hr>|<h[123]>)', doc, re.S):
        name = html.unescape(m.group(1))
        sig = html.unescape(re.sub(r'<[^>]+>', '', m.group(2))).replace('···', '...').replace('·', '')
        sig = sig.replace('&middot;', '')
        entries[name] = (sig.strip(), html_to_md(m.group(3)))
    return entries


def lua_names():
    p = subprocess.run(['lua', '-'], input=ENUM, capture_output=True, text=True, check=True)
    names = {}
    for line in p.stdout.splitlines():
        n, k = line.split('\t')
        names[n] = k
    return names


def comment_block_above(lines, i):
    """The contiguous // and /** */ comment lines above line i (exclusive)."""
    j = i - 1
    block = []
    while j >= 0:
        s = lines[j].strip()
        if s.startswith('//') or s.startswith('*') or s.startswith('/**') or s.endswith('*/'):
            block.append(s)
            j -= 1
            continue
        break
    block.reverse()
    return block


def clean_comment(block):
    out = []
    for s in block:
        s = re.sub(r'^(///?|/\*\*|\*/|\*)\s?', '', s)
        s = s.rstrip('*/').rstrip()
        out.append(s)
    return out


def mep_bindings():
    src = (REPO / 'src' / 'lua_env.cpp').read_text()
    lines = src.splitlines()
    table = src[src.index('const luaL_Reg kMepFuncs[]'):]
    table = table[:table.index('};')]
    defs = {}
    for i, line in enumerate(lines):
        m = re.match(r'(?:static\s+)?int\s+(l_\w+)\s*\(\s*lua_State', line)
        if m:
            defs[m.group(1)] = i
    # Every `// mep.x(args) -> ret. prose` paragraph anywhere in the file: one
    # C function sometimes documents several bindings in one comment block.
    segs = {}
    i = 0
    while i < len(lines):
        if not lines[i].strip().startswith('//'):
            i += 1
            continue
        j = i
        while j < len(lines) and lines[j].strip().startswith('//'):
            j += 1
        text = re.sub(r'\s+', ' ', ' '.join(re.sub(r'^\s*//\s?', '', l) for l in lines[i:j]))
        starts = list(re.finditer(r'(?:^|(?<=[.;:] ))mep\.(\w+)(\((?:[^()]|\([^()]*\))*\))(\s*->\s*[^.;:]*)?\.?\s*', text))
        for k, m in enumerate(starts):
            stop = starts[k + 1].start() if k + 1 < len(starts) else len(text)
            if m.group(1) not in segs:
                segs[m.group(1)] = (m.group(2) + ((' ' + m.group(3).strip()) if m.group(3) else ''),
                                    text[m.end():stop].strip())
        i = j
    out = {}
    for name, fn in re.findall(r'\{\s*"(\w+)"\s*,\s*(l_\w+)\s*\}', table):
        sig, doc = '(...)', ''
        if fn in defs:
            block = clean_comment(comment_block_above(lines, defs[fn]))
            plain, doxy = [], []
            for l in block:
                (doxy if doxy or l.startswith('@') else plain).append(l)
            ptext = re.sub(r'\s+', ' ', ' '.join(plain)).strip()
            dtext = re.sub(r'\s+', ' ', ' '.join(doxy)).strip()
            ret = ''
            m = re.search(r'mep\.' + re.escape(name) + r'(\((?:[^()]|\([^()]*\))*\))(\s*->\s*[^.;:]*)?\.?\s*', ptext)
            if m:
                sig = m.group(1)
                ret = (m.group(2) or '').strip()
                ptext = (ptext[:m.start()] + ptext[m.end():]).strip()
            b = re.search(r'@brief\s+(.*?)(?=\s@\w|\Z)', dtext)
            brief = b.group(1).strip() if b else ''
            bm = re.match(r'Implements mep\.' + re.escape(name) + r'(\((?:[^()]|\([^()]*\))*\))?\s*:?\s*', brief)
            if bm:
                if bm.group(1) and sig == '(...)':
                    sig = bm.group(1)
                brief = brief[bm.end():]
            if ret:
                sig += ' ' + ret
            doc = ptext if len(ptext) >= len(brief) else brief
        if name in segs:
            sig, sdoc = segs[name]
            if len(sdoc) > 20 or not doc or doc.lower().startswith('mep.'):
                doc = sdoc
        doc = doc.lstrip(': ').strip()
        if doc.lower().startswith('mep.'):
            doc = ''
        if doc:
            doc = doc[0].upper() + doc[1:]
        out[name] = (sig, doc[:1500])
    return out


def mep_lua_defined():
    src = (REPO / 'src' / 'main.cpp').read_text()
    lines = src.splitlines()
    out = {}
    pat = [re.compile(r'^\s*"\s*function mep\.(\w+)\s*\(([^)]*)\)'),
           re.compile(r'^\s*"\s*mep\.(\w+)\s*=\s*function\s*\(([^)]*)\)')]
    for i, line in enumerate(lines):
        for p in pat:
            m = p.match(line)
            if m and m.group(1) not in out:
                block = [l for l in clean_comment(comment_block_above(lines, i))]
                doc = re.sub(r'\s+', ' ', ' '.join(block)).strip()
                out[m.group(1)] = ('(' + m.group(2).strip() + ')', doc[:1200])
    # Tables and values assigned at the top level of a chunk: mep.opt, mep.lsp_servers, ...
    for m in re.finditer(r'^\s*"(mep\.(\w+))\s*=\s*([{\w\'"])', src, re.M):
        name = m.group(2)
        if name not in out:
            out[name] = ('', '')
    return out


def main():
    if len(sys.argv) < 2:
        sys.exit('usage: gen_lua_vocab.py PATH/TO/lua-5.4/doc/manual.html')
    manual = manual_entries(sys.argv[1])
    names = lua_names()
    version = subprocess.run(['lua', '-v'], capture_output=True, text=True).stdout.strip()
    rows = []
    for n in sorted(names):
        if n.startswith('_G.') or n == 'arg':
            continue
        k = names[n]
        sig, doc = manual.get(n, ('', ''))
        if sig.startswith(n):
            sig = sig[len(n):].strip()
        rows.append(('std', n, sig, k, doc))
    # File-handle methods (file:read, ...) appear only in the manual.
    for n, (sig, doc) in sorted(manual.items()):
        if n.startswith('file:'):
            rows.append(('std', n, sig[len(n):].strip() if sig.startswith(n) else sig, 'function', doc))
    for n, why in sorted(OTHER_VERSIONS.items()):
        if n not in names:
            rows.append(('old', n, '', 'function' if '.' in n or n[0].islower() else 'value', why))
    rows.append(('mep', 'mep', '', 'table', "mep's own API: the editor, its buffers, panes and languages."))
    api = mep_bindings()
    for n, (sig, doc) in sorted(api.items()):
        rows.append(('mep', 'mep.' + n, sig, 'function', doc))
    for n, (sig, doc) in sorted(mep_lua_defined().items()):
        if n not in api:
            rows.append(('mep', 'mep.' + n, sig, 'function' if sig else 'value', doc))

    out = ['// Generated by tools/gen_lua_vocab.py -- do not edit.',
           f'// Lua standard library from {version} and its reference manual:',
           '// Copyright (c) 1994-2024 Lua.org, PUC-Rio. Licensed under the MIT license.',
           "// mep's API from src/lua_env.cpp (kMepFuncs) and src/main.cpp's builtin chunks.",
           '#include "lua_lsp_vocab.h"', '', 'namespace {', '', 'const LuaLspApi kApi[] = {']
    for src, n, sig, k, doc in rows:
        out.append(f'    {{{cstr(src)}, {cstr(n)}, {cstr(sig)}, {cstr(k)}, {cstr(doc)}}},')
    out += ['};', '', '}  // namespace', '',
            'const LuaLspApi *LuaLspApiTable(size_t *count) {',
            '    *count = sizeof(kApi) / sizeof(*kApi);', '    return kApi;', '}', '']
    OUT.write_text('\n'.join(out))
    print(f'{len(rows)} names -> {OUT.relative_to(REPO)}', file=sys.stderr)


if __name__ == '__main__':
    main()
