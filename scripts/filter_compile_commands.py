#!/usr/bin/env python3
"""Filter compile_commands.json down to mep's own top-level src/*.{c,cpp}
entries, dropping every vendored/fetched translation unit (third_party/,
build/*/_deps/, generated test/example scaffolding). Cppcheck's own -i
path-ignore flag is silently a no-op in --project mode (verified against
cppcheck 2.18.3 -- it still analyzes ignored paths), so this exists as the
actual filtering mechanism `just lint-cppcheck` feeds cppcheck instead.
"""
import json
import re
import sys

src, dst = sys.argv[1], sys.argv[2]
with open(src) as f:
    entries = json.load(f)

pattern = re.compile(r"/mep/src/[^/]+\.(c|cpp)$")
own = [e for e in entries if pattern.search(e["file"])]

with open(dst, "w") as f:
    json.dump(own, f)

print(f"{len(own)} of {len(entries)} compile-db entries are mep's own src/", file=sys.stderr)
