#!/bin/sh
# Does Crystal's own parser accept the file $1? grammar_diff.py's oracle
# for Crystal. `crystal tool format` parses (no semantic pass, no
# require'd files) before it formats, so a syntax error is the parser's.
# The formatter can also fail on code that parsed ("there's a bug in the
# formatter"): that file is valid, so only "syntax error" rejects it.
err=$(crystal tool format - < "$1" 2>&1 >/dev/null) && exit 0
case "$err" in
  *"syntax error"* | *"Syntax error"*) exit 1 ;;
esac
exit 0
