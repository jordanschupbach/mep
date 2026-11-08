#!/usr/bin/env python3
"""Does rustc's parser accept a Rust file? The oracle for `just grammar-diff
rust --oracle`, mep-rust-lsp's syntax-error parity and format_compare.py.

    rust_check.py FILE...

Runs `rustc -Zparse-crate-root-only` (stable rustc with RUSTC_BOOTSTRAP=1):
the file is lexed and parsed -- macro bodies stay token trees, `mod x;` is
not followed, nothing is expanded, resolved or type-checked -- so the
answer is exactly "is this syntactically Rust". The edition matters (`async`,
`dyn`, `gen`, `try` are keywords only in later ones); it comes from a
compiletest `//@ edition:` header, else the nearest Cargo.toml's `edition`
(following `edition.workspace = true` up to the workspace's
`[workspace.package]`), else 2021.

Prints `FILE:LINE:COL: message` for the first error of each rejected file;
exits 1 if any file is rejected.
"""

import os
import re
import subprocess
import sys
from pathlib import Path

EDITION_HEADER = re.compile(r'^\s*//@\s*(?:compile-flags:.*--edition[= ]|edition:\s*)(\d{4})', re.M)
EDITION_KEY = re.compile(r'^\s*edition\s*=\s*"(\d{4})"', re.M)
WORKSPACE_EDITION = re.compile(r'^\s*edition(?:\s*\.\s*workspace\s*=\s*true|\s*=\s*\{\s*workspace\s*=\s*true)', re.M)
ERROR = re.compile(r'^error(?:\[E\d+\])?: (.*)\n\s*--> (.*?):(\d+):(\d+)', re.M)

_toml_cache = {}


def cargo_edition(path):
    """The edition of the crate holding `path`, or None."""
    want_workspace = False
    d = Path(path).resolve().parent
    while True:
        toml = d / 'Cargo.toml'
        if toml.is_file():
            if toml not in _toml_cache:
                try:
                    _toml_cache[toml] = toml.read_text(errors='replace')
                except OSError:
                    _toml_cache[toml] = ''
            text = _toml_cache[toml]
            if want_workspace:
                if '[workspace' in text:
                    m = EDITION_KEY.search(text.split('[workspace.package]', 1)[1]) if '[workspace.package]' in text else None
                    if m:
                        return m.group(1)
            else:
                m = EDITION_KEY.search(text)
                if m:
                    return m.group(1)
                if WORKSPACE_EDITION.search(text):
                    want_workspace = True
                elif '[package]' in text:
                    return '2015'  # Cargo's default when a package names none
        if d.parent == d:
            return None
        d = d.parent


def edition(path):
    try:
        head = Path(path).read_text(errors='replace')[:4000]
    except OSError:
        head = ''
    m = EDITION_HEADER.search(head)
    if m:
        return m.group(1)
    return cargo_edition(path) or '2021'


def check(path):
    """None if rustc accepts the file, else 'LINE:COL: message'."""
    env = dict(os.environ, RUSTC_BOOTSTRAP='1')
    got = subprocess.run(['rustc', '-Zparse-crate-root-only', '--edition', edition(path), '--crate-type', 'lib',
                          '--error-format', 'human', '--color', 'never', '-o', os.devnull, path],
                         capture_output=True, text=True, env=env, timeout=120)
    if got.returncode == 0:
        return None
    m = ERROR.search(got.stderr)
    if m:
        return f'{m.group(3)}:{m.group(4)}: {m.group(1)}'
    first = got.stderr.strip().splitlines()[:1]
    return '0:0: ' + (first[0] if first else f'rustc exited {got.returncode}')


def main(argv):
    bad = 0
    for path in argv[1:]:
        err = check(path)
        if err is not None:
            bad += 1
            print(f'{path}:{err}')
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv))
