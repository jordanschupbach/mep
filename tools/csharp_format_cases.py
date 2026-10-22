#!/usr/bin/env python3
"""Extract test cases for mep's C# formatter from Roslyn's own formatting tests.

    python3 -I tools/csharp_format_cases.py ROSLYN_FORMATTING_TEST_DIR ORACLE_DLL OUT.inc

ROSLYN_FORMATTING_TEST_DIR is Roslyn's src/Workspaces/CSharpTest/Formatting
(at the commit the SDK's dotnet-format was built from). Every test that is a
plain `AssertFormatAsync(expected, input)` -- two string literals, no options,
no spans -- becomes a case, kept only when Roslyn's formatter as dotnet format
runs it (`csharp-oracle format`, default options) turns the input into the
expected text exactly: tests that rely on the test harness's own settings drop
out. The result is src/csharp_format_test_cases.inc (mep-csharp-format-test).
"""

import os
import re
import subprocess
import sys
import tempfile


def read_literal(s, i):
    """Parse a C# string literal at s[i:]; return (value, end) or None."""
    if s.startswith('$', i) or s.startswith('@$', i):
        return None
    if s.startswith('@"', i):
        j = i + 2
        out = []
        while j < len(s):
            if s[j] == '"':
                if s.startswith('""', j):
                    out.append('"')
                    j += 2
                    continue
                return ''.join(out), j + 1
            out.append(s[j])
            j += 1
        return None
    m = re.match(r'"{3,}', s[i:])
    if m:
        q = m.group(0)
        j = i + len(q)
        nl = s.find('\n', j)
        if nl >= 0 and s[j:nl].strip() == '':
            # Multi-line raw string: content lines, the closing quotes' indentation removed.
            end = s.find(q, nl)
            if end < 0:
                return None
            line_start = s.rfind('\n', 0, end) + 1
            indent = s[line_start:end]
            if indent.strip() != '':
                return None
            body = s[nl + 1:line_start]
            lines = body.split('\n')[:-1] if body.endswith('\n') else body.split('\n')
            out = []
            for line in lines:
                line = line.rstrip('\r')
                if line.strip() == '':
                    out.append('')
                elif line.startswith(indent):
                    out.append(line[len(indent):])
                else:
                    return None
            return '\n'.join(out), end + len(q)
        end = s.find(q, j)
        if end < 0:
            return None
        return s[j:end], end + len(q)
    if s.startswith('"', i):
        j = i + 1
        out = []
        esc = {'n': '\n', 'r': '\r', 't': '\t', '"': '"', '\\': '\\', '0': '\0', "'": "'"}
        while j < len(s):
            c = s[j]
            if c == '\\':
                d = s[j + 1]
                if d in esc:
                    out.append(esc[d])
                    j += 2
                    continue
                if d == 'u':
                    out.append(chr(int(s[j + 2:j + 6], 16)))
                    j += 6
                    continue
                return None
            if c == '"':
                return ''.join(out), j + 1
            if c == '\n':
                return None
            out.append(c)
            j += 1
    return None


def skip_ws(s, i):
    while i < len(s):
        if s[i].isspace():
            i += 1
        elif s.startswith('//', i):
            i = s.find('\n', i)
            if i < 0:
                return len(s)
        else:
            break
    return i


def extract(path):
    s = open(path, encoding='utf-8-sig').read()
    cases = []
    for m in re.finditer(r'public (?:async )?Task (\w+)\(\)', s):
        name = m.group(1)
        nxt = re.search(r'\n    \[(?:Fact|Theory|WpfFact)', s[m.end():])
        body = s[m.end():m.end() + (nxt.start() if nxt else len(s))]
        calls = [c.end() for c in re.finditer(r'\bAssertFormatAsync\(', body)]
        if len(calls) != 1:
            continue
        i = skip_ws(body, calls[0])
        a = read_literal(body, i)
        if not a:
            continue
        i = skip_ws(body, a[1])
        if not body.startswith(',', i):
            continue
        b = read_literal(body, skip_ws(body, i + 1))
        if not b:
            continue
        i = skip_ws(body, b[1])
        if not body.startswith(')', i):
            continue
        cases.append((os.path.basename(path)[:-3] + '.' + name, a[0], b[0]))
    return cases


def c_literal(text):
    out = ['"']
    for ch in text:
        o = ord(ch)
        if ch == '\\':
            out.append('\\\\')
        elif ch == '"':
            out.append('\\"')
        elif ch == '\n':
            out.append('\\n"\n    "')
        elif ch == '\r':
            out.append('\\r')
        elif ch == '\t':
            out.append('\\t')
        elif o < 0x20:
            out.append('\\x%02x""' % o)
        elif o < 0x80:
            out.append(ch)
        else:
            out.append(''.join('\\x%02x""' % b for b in ch.encode('utf-8')))
    out.append('"')
    return ''.join(out).replace('\n    ""', '')


def main():
    src_dir, oracle, out_path = sys.argv[1:4]
    cases = []
    for f in sorted(os.listdir(src_dir)):
        if f.endswith('.cs'):
            cases.extend(extract(os.path.join(src_dir, f)))
    kept = []
    with tempfile.TemporaryDirectory() as tmp:
        paths = []
        for k, (name, expected, code) in enumerate(cases):
            p = os.path.join(tmp, '%d.cs' % k)
            with open(p, 'w', encoding='utf-8', newline='') as fh:
                fh.write(code)
            paths.append(p)
        outdir = os.path.join(tmp, 'out')
        subprocess.run(['dotnet', oracle, 'format', '--out', outdir, *paths], check=True, stdout=subprocess.DEVNULL)
        for k, (name, expected, code) in enumerate(cases):
            got = open(os.path.join(outdir, paths[k].lstrip('/')), encoding='utf-8', newline='').read()
            if got == expected and code != expected:
                kept.append((name, code, expected))
    with open(out_path, 'w', encoding='utf-8') as o:
        o.write('// GENERATED by tools/csharp_format_cases.py from Roslyn\'s src/Workspaces/CSharpTest/Formatting\n')
        o.write('// (MIT, Copyright (c) .NET Foundation and Contributors): %d of the %d plain\n' % (len(kept), len(cases)))
        o.write('// AssertFormatAsync(expected, input) tests -- those `csharp-oracle format` agrees with. Do not edit.\n')
        for name, code, expected in kept:
            o.write('{"%s",\n    %s,\n    %s},\n' % (name, c_literal(code), c_literal(expected)))
    print('%d cases extracted, %d kept' % (len(cases), len(kept)))


if __name__ == '__main__':
    main()
