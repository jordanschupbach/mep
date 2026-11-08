#!/usr/bin/env python3
"""Runic.jl's output for one Julia file, from the cache that
tools/julia_runic_cache.jl fills -- the reference command of
format_compare.py's 'julia' preset.

    julia_runic_ref.py FILE

Prints what `runic FILE` would print and exits 0, or exits 1 when Runic
refuses the file (its message on stderr). On a cache miss it runs
julia_runic_cache.jl for this one file first (slow: Runic's start-up), with
the julia from $MEP_RUNIC_JULIA, else `julia` on PATH (it needs Runic in its
load path; e.g. nixpkgs julia-bin.withPackages ["Runic"]).

Fill the cache for a whole corpus in one go (one julia process per shard):

    julia tools/julia_runic_cache.jl build/grammar-corpus/julia/files.txt
"""

import hashlib
import os
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CACHE = REPO / 'build' / 'julia' / 'runic-cache'


def main():
    path = sys.argv[1]
    data = Path(path).read_bytes()
    h = hashlib.sha256(data).hexdigest()
    out, err = CACHE / f'{h}.out', CACHE / f'{h}.err'
    if not out.exists() and not err.exists():
        julia = os.environ.get('MEP_RUNIC_JULIA', 'julia')
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
            f.write(os.path.abspath(path) + '\n')
            lst = f.name
        try:
            env = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}
            subprocess.run([julia, str(REPO / 'tools' / 'julia_runic_cache.jl'), lst, str(CACHE)],
                           env=env, capture_output=True)
        finally:
            os.unlink(lst)
    if out.exists():
        sys.stdout.buffer.write(out.read_bytes())
        return 0
    if err.exists():
        sys.stderr.write(err.read_text(errors='replace'))
        return 1
    sys.stderr.write('julia_runic_ref: Runic did not run (set MEP_RUNIC_JULIA)\n')
    return 2


if __name__ == '__main__':
    sys.exit(main())
