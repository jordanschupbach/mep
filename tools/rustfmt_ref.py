#!/usr/bin/env python3
"""rustfmt as format_compare.py's reference for the `rust` preset.

    rustfmt_ref.py FILE            # print rustfmt's output for FILE
    rustfmt_ref.py --fill LIST     # fill the cache for every file in LIST

rustfmt 1.97.1 (nixpkgs#rustfmt, pinned by mep's flake) with its default
configuration: the file goes through stdin (so `mod x;` is not followed) with
`--edition` from tools/ts_grammar_diff/rust_check.py's edition() and
`--config-path` pointing at an empty rustfmt.toml, so the corpus repos' own
rustfmt.toml files (rustc's tree has one) never apply. A file rustfmt fails
on, or reports any error or warning for, exits 1: there is nothing to compare.

Outputs are cached by (content hash, edition) under
build/rust/scratch/fmtref, so repeated comparisons only run mep's side.
"""

import concurrent.futures
import hashlib
import os
import subprocess
import sys
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(REPO / 'tools' / 'ts_grammar_diff'))
import rust_check  # noqa: E402

CACHE = REPO / 'build' / 'rust' / 'scratch' / 'fmtref'
EMPTY_CFG = REPO / 'build' / 'rust' / 'scratch' / 'emptycfg' / 'rustfmt.toml'


def rustfmt():
    exe = os.environ.get('RUSTFMT')
    if exe:
        return exe
    out = subprocess.run(['nix', 'build', '--no-link', '--print-out-paths', 'nixpkgs#rustfmt',
                          '--inputs-from', str(REPO)], check=True, capture_output=True, text=True).stdout.split()
    return str(Path(out[-1]) / 'bin' / 'rustfmt')


def reference(path, exe):
    """(ok, output bytes) for one file, cached."""
    data = Path(path).read_bytes()
    edition = rust_check.edition(path)
    key = hashlib.sha256(data + b'\0' + edition.encode()).hexdigest()
    hit = CACHE / key[:2] / key
    bad = hit.with_suffix('.fail')
    if hit.exists():
        return True, hit.read_bytes()
    if bad.exists():
        return False, b''
    EMPTY_CFG.parent.mkdir(parents=True, exist_ok=True)
    EMPTY_CFG.touch()
    try:
        p = subprocess.run([exe, '--edition', edition, '--config-path', str(EMPTY_CFG)], input=data,
                           capture_output=True, timeout=120)
        ok = p.returncode == 0 and not p.stderr.strip()
    except subprocess.TimeoutExpired:
        ok, p = False, None
    hit.parent.mkdir(parents=True, exist_ok=True)
    if ok:
        tmp = hit.with_suffix('.tmp%d' % os.getpid())
        tmp.write_bytes(p.stdout)
        tmp.replace(hit)
        return True, p.stdout
    bad.write_bytes(p.stderr if p else b'timeout')
    return False, b''


def main(argv):
    exe = rustfmt()
    if len(argv) == 3 and argv[1] == '--fill':
        files = [f for f in Path(argv[2]).read_text().split('\n') if f]
        good = 0
        with concurrent.futures.ThreadPoolExecutor(os.cpu_count()) as ex:
            for k, (ok, _) in enumerate(ex.map(lambda f: reference(f, exe), files), 1):
                good += ok
                if k % 1000 == 0:
                    print(f'  {k}/{len(files)}', file=sys.stderr)
        print(f'{good} of {len(files)} formatted cleanly by rustfmt')
        return 0
    ok, out = reference(argv[1], exe)
    if not ok:
        return 1
    sys.stdout.buffer.write(out)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
