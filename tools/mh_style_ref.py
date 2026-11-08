#!/usr/bin/env python3
"""MISS_HIT's `mh_style --fix` as a filter, for tools/format_compare.py.

    python3 tools/mh_style_ref.py FILE.m     # the fixed text on stdout

mh_style rewrites files in place, so FILE is copied (under its own name: a
classdef must match it) into a temporary directory and fixed there. Default
rules, no miss_hit.cfg (--ignore-config), Octave syntax (--octave latest).
Exit 1, with mh_style's error on stderr, when it does not fix the file (it
refuses anything its parser rejects). mh_style comes from PATH, else
$MH_STYLE, else `nix build nixpkgs#python3Packages.miss-hit-core`.
"""

import os
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path


def mh_style():
    exe = os.environ.get('MH_STYLE') or shutil.which('mh_style')
    if exe:
        return exe
    out = subprocess.run(['nix', 'build', '--no-link', '--print-out-paths', 'nixpkgs#python3Packages.miss-hit-core'],
                         check=True, capture_output=True, text=True).stdout.split()
    return str(Path(out[-1]) / 'bin' / 'mh_style')


def main():
    src = Path(sys.argv[1])
    with tempfile.TemporaryDirectory() as d:
        dst = Path(d) / src.name
        shutil.copyfile(src, dst)
        env = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}
        p = subprocess.run([mh_style(), '--octave', 'latest', '--ignore-config', '--fix', '--brief', str(dst)],
                           capture_output=True, text=True, env=env)
        # A lex error stops mh_style before fixing; a parse error makes it say
        # "not auto-fixed". Other errors (`break` outside a loop) are not fatal.
        # A crash (a Python traceback, an ICE) leaves the file as it was.
        errors = [ln for ln in (p.stdout + p.stderr).splitlines()
                  if 'lex error' in ln or 'not auto-fixed' in ln or 'Traceback' in ln or 'ICE' in ln]
        if p.returncode not in (0, 1) or errors:
            sys.stderr.write('\n'.join(errors) or p.stderr)
            return 1
        sys.stdout.buffer.write(dst.read_bytes())
        return 0


if __name__ == '__main__':
    sys.exit(main())
