#!/usr/bin/env bash
# The D oracle for `just grammar-diff d --oracle`: exit 0 when the D
# compiler's own parser (LDC's copy of the dmd front end) reads the file
# without a syntax error. `ldc2 -o- -v` parses every root module first and
# stops before "importall" if the parser reported anything, so a run that
# reached importall had no syntax error, whatever semantic analysis said
# afterwards (unresolved imports, type errors). Nothing is code-generated.
# -unittest: without it the parser skips unittest bodies unread.
# Uses $LDC2, else the ldc `just d-check` links into build/grammar-diff/d,
# else `ldc2` on PATH.
here=$(cd "$(dirname "$0")" && pwd)
ldc=${LDC2:-$here/../../build/grammar-diff/d/ldc/bin/ldc2}
[ -x "$ldc" ] || ldc=ldc2
out=$("$ldc" -o- -v -c -unittest "$1" 2>/dev/null)
rc=$?
[ $rc -eq 0 ] && exit 0
grep -q '^importall' <<<"$out" && exit 0
exit 1
