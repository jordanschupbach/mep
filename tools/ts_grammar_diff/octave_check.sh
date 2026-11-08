#!/bin/sh
# Does Octave's own parser accept this .m file? __parse_file__ parses it
# (function file, script or classdef) and runs nothing. Exit 0 iff it does;
# the parse error goes to stderr.
f=$1
[ -n "$f" ] || { echo "usage: octave_check.sh FILE" >&2; exit 2; }
OCTAVE_CHECK_FILE=$f exec env -u LD_LIBRARY_PATH octave-cli --no-gui --quiet --norc --no-history --eval '
try
  __parse_file__ (getenv ("OCTAVE_CHECK_FILE"));
catch err
  fputs (stderr, [err.message "\n"]);
  exit (1);
end_try_catch'
