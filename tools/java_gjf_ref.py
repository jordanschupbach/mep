#!/usr/bin/env python3
"""google-java-format 1.36.1's output for one Java file, from the cache that
tools/java_gjf_cache.java fills -- the reference command of
format_compare.py's 'java' preset.

    java_gjf_ref.py FILE

Prints what `google-java-format FILE` would print and exits 0, or exits 1
when GJF refuses the file (its diagnostics on stderr). On a cache miss it
runs java_gjf_cache.java for this one file first (slow: a JVM start-up).
GJF comes from $MEP_GJF (its bin/google-java-format wrapper), else
build/java/scratch/roots/gjf, else `google-java-format` on PATH.

Fill the cache for a whole corpus in one JVM:

    java_gjf_ref.py --fill build/grammar-corpus/java/files.txt
"""

import hashlib
import os
import re
import shutil
import subprocess
import sys
import tempfile
from pathlib import Path

REPO = Path(__file__).resolve().parents[1]
CACHE = REPO / 'build' / 'java' / 'scratch' / 'fmt' / 'cache'


def gjf_command():
    """The wrapper's java + flags + jar, as a classpath command for java_gjf_cache.java."""
    wrapper = os.environ.get('MEP_GJF')
    if not wrapper:
        local = REPO / 'build' / 'java' / 'scratch' / 'roots' / 'gjf' / 'bin' / 'google-java-format'
        wrapper = str(local) if local.exists() else shutil.which('google-java-format')
    if not wrapper:
        sys.exit('java_gjf_ref: google-java-format not found (set MEP_GJF)')
    text = Path(wrapper).read_text()
    m = re.search(r'exec -a "google-java-format" "([^"]+)"\s+(.*?)-jar (\S+)', text)
    if not m:
        sys.exit(f'java_gjf_ref: cannot read the wrapper {wrapper}')
    return [m.group(1), *m.group(2).split(), '-cp', m.group(3), str(REPO / 'tools' / 'java_gjf_cache.java')]


def fill(listfile):
    env = {k: v for k, v in os.environ.items() if k != 'LD_LIBRARY_PATH'}
    subprocess.run(gjf_command() + [listfile, str(CACHE)], env=env, check=True)


def main():
    if sys.argv[1] == '--fill':
        fill(sys.argv[2])
        return 0
    path = sys.argv[1]
    data = Path(path).read_bytes()
    h = hashlib.sha256(data).hexdigest()
    out, err = CACHE / f'{h}.out', CACHE / f'{h}.err'
    if not out.exists() and not err.exists():
        with tempfile.NamedTemporaryFile('w', suffix='.txt', delete=False) as f:
            f.write(os.path.abspath(path) + '\n')
            lst = f.name
        try:
            fill(lst)
        finally:
            os.unlink(lst)
    if out.exists():
        sys.stdout.buffer.write(out.read_bytes())
        return 0
    if err.exists():
        sys.stderr.write(err.read_text(errors='replace'))
        return 1
    sys.stderr.write('java_gjf_ref: google-java-format did not run\n')
    return 2


if __name__ == '__main__':
    sys.exit(main())
