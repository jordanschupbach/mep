#!/usr/bin/env python3
"""GAP's own reader as an oracle: does GAP accept these files?

    python3 -I gap_check.py FILE              exit 0 if GAP reads it, 1 if not
    python3 -I gap_check.py --report FILE...  one line per diagnostic, as
                                              `FILE:LINE:COL: error|warning: MESSAGE`,
                                              then `FILE: accepted|rejected|aborted`
    --toplevel LSP                            see below (before the other arguments)

Each file goes through `ReadAsFunction`, which runs GAP's reader (src/read.c)
over the whole file and codes it as one function body -- nothing in it is
executed. One GAP process reads many files; GAP prints a syntax error or
warning as

    Syntax error: <message> in <file>:<line>
    <the source line>
    <spaces>^

and that is parsed back here (COL is the caret's, 1-based; 0 when GAP
printed none). "aborted" is a file GAP gave up on with a run-time error
while reading (an eager float literal such as `1.5_x` is converted by the
reader): no verdict.

Some input leaves GAP's reader in a state where it reports nothing for the
files after it (`atomic function() end;` as a statement does). So after
every file a canary with a known error is read; when its error does not
come, the files after it are read again in a fresh GAP.

A file's top level is not quite a function body: a bare expression
statement (`x;`, `1+1;`) is fine in a file GAP `Read`s and an error inside a
function, and `quit;`/`?topic` only work at the top level. With
--toplevel LSP, each file is first passed through `LSP --gap-toplevel FILE`,
which prints `E OFFSET` where a top-level expression statement starts and
`B START END` for a top-level `quit;`/`QUIT;`/`?topic` (mep-gap-lsp's own
parser decides both). `GapCheckExpr__ := ` is inserted at each E and each B
range is blanked before GAP reads the file, so GAP checks the rest under
the rules a `Read` would use. Line numbers are unchanged; columns on those
lines move.
"""

import os
import re
import subprocess
import sys
import tempfile

GAP = os.environ.get('GAP', 'gap')
PREFIX = b'GapCheckExpr__ := '
CANARY_TEXT = 'GapCheckCanary__ := 1 +;\n'
LINE_RE = re.compile(r'^Syntax (error|warning): (.*) in (.*):(\d+)$')


def gap_string(s):
    return '"' + s.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def rewrite(data, edits):
    """Insert PREFIX at each ('E', off) and blank each ('B', start, end), keeping newlines."""
    out, last = [], 0
    for e in sorted(edits, key=lambda e: e[1]):
        if e[1] < last:
            continue
        out.append(data[last:e[1]])
        if e[0] == 'E':
            out.append(PREFIX)
            last = e[1]
        else:
            out.append(bytes(b if b == 10 else 32 for b in data[e[1]:e[2]]))
            last = e[2]
    out.append(data[last:])
    return b''.join(out)


def prepare(files, toplevel, tmpdir):
    """The paths GAP reads (a rewritten copy for --toplevel files)."""
    if not toplevel:
        return list(files)
    out = []
    for i, f in enumerate(files):
        got = subprocess.run([toplevel, '--gap-toplevel', f], capture_output=True)
        edits = []
        if got.returncode == 0:
            for line in got.stdout.decode().splitlines():
                parts = line.split()
                if parts and parts[0] == 'E':
                    edits.append(('E', int(parts[1])))
                elif parts and parts[0] == 'B':
                    edits.append(('B', int(parts[1]), int(parts[2])))
        if not edits:
            out.append(f)
            continue
        path = os.path.join(tmpdir, f'{i}.g')
        with open(path, 'wb') as fh:
            fh.write(rewrite(open(f, 'rb').read(), edits))
        out.append(path)
    return out


def run_batch(paths, canary):
    """Reads `paths` in one GAP: ({path: (diags, verdict)}, how many paths it got through)."""
    prog = ['SizeScreen([4096, 24]);;', 'BreakOnError := false;;']
    for p in paths:
        prog.append(f'Print("@@FILE ", {gap_string(p)}, "\\n");; '
                    f'if ReadAsFunction({gap_string(p)}) = fail then Print("@@FAIL\\n"); fi; '
                    f'Print("@@END\\n");; ReadAsFunction({gap_string(canary)});; Print("@@CANARY\\n");;')
    prog.append('QUIT;')
    got = subprocess.run([GAP, '-q', '-A', '-b', '-x', '4096'], input='\n'.join(prog) + '\n',
                         stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors='replace',
                         timeout=7200)
    results, done = {}, 0
    current, diags, verdict, canary_seen, runtime_error = None, [], 'accepted', False, False
    lines = got.stdout.splitlines()
    i = 0
    while i < len(lines):
        line = lines[i]
        if line.startswith('@@FILE '):
            current, diags, verdict, runtime_error = line[7:], [], 'accepted', False
        elif line == '@@FAIL':
            verdict = 'rejected'
        elif line == '@@END':
            results[current] = (diags, 'aborted' if runtime_error else verdict)
            canary_seen = False
        elif line == '@@CANARY':
            done += 1
            if not canary_seen:
                return results, done  # GAP's reader is in a bad state: start again after this file
        elif line.startswith('Error, ') and current is not None:
            runtime_error = True
        else:
            m = LINE_RE.match(line)
            if m:
                if m.group(3) == canary:
                    canary_seen = True
                else:
                    col = 0
                    if i + 2 < len(lines) and lines[i + 2].strip().startswith('^'):
                        col = lines[i + 2].index('^') + 1  # GAP repeats the line's tabs before the caret
                        i += 2
                    diags.append((int(m.group(4)), col, m.group(1), m.group(2)))
        i += 1
    if current is not None and current not in results:
        results[current] = ([], 'aborted')  # GAP died reading it
        done += 1
    return results, done


def run(files, toplevel=None):
    """[(file, line, col, kind, message)] over every file, in order, and
    {file: 'accepted' | 'rejected' | 'aborted'}."""
    with tempfile.TemporaryDirectory(prefix='gapchk') as tmpdir:
        canary = os.path.join(tmpdir, 'canary.g')
        with open(canary, 'w') as fh:
            fh.write(CANARY_TEXT)
        paths = prepare(files, toplevel, tmpdir)
        back = dict(zip(paths, files))
        results, start = {}, 0
        while start < len(paths):
            got, done = run_batch(paths[start:], canary)
            results.update(got)
            start += max(done, 1)
    diags, verdicts = [], {}
    for p in paths:
        ds, verdict = results.get(p, ([], 'aborted'))
        verdicts[back[p]] = verdict
        diags += [(back[p], *d) for d in ds]
    return diags, verdicts


def main(argv):
    toplevel = None
    if argv and argv[0] == '--toplevel':
        toplevel, argv = argv[1], argv[2:]
    if argv and argv[0] == '--report':
        diags, verdicts = run(argv[1:], toplevel)
        for f, line, col, kind, msg in diags:
            print(f'{f}:{line}:{col}: {kind}: {msg}')
        for f in argv[1:]:
            print(f'{f}: {verdicts[f]}')
        return 0
    if len(argv) != 1:
        print(__doc__, file=sys.stderr)
        return 2
    _, verdicts = run(argv, toplevel)
    return 0 if verdicts[argv[0]] == 'accepted' else 1


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
