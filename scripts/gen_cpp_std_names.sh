#!/usr/bin/env bash
# Regenerates src/cpp_lsp_std_names.cpp from the real standard library
# headers on this machine. Run from the repository root, in an
# environment with a C++20 compiler on PATH:
#
#   nix develop --command scripts/gen_cpp_std_names.sh
#
# The generated file is committed, so building mep never needs those
# headers; re-run this only to refresh the list against a newer standard
# library. See scripts/gen_cpp_std_names.cpp for what is extracted and
# why the extraction is a parse rather than a regex.
set -euo pipefail

root="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
work="$(mktemp -d)"
trap 'rm -rf "$work"' EXIT

cxx="${CXX:-g++}"

# Every header [headers] lists, plus the C compatibility set. A header a
# particular library does not have is skipped rather than failing the
# run, which is what the per-header probe below is for.
headers=(
  algorithm any array atomic barrier bit bitset charconv chrono codecvt compare complex concepts
  condition_variable coroutine deque exception execution expected filesystem format forward_list
  fstream functional future initializer_list iomanip ios iosfwd iostream istream iterator latch
  limits list locale map mdspan memory memory_resource mutex new numbers numeric optional ostream
  print queue random ranges ratio regex scoped_allocator semaphore set shared_mutex source_location
  span spanstream sstream stack stacktrace stdexcept stop_token streambuf string string_view
  syncstream system_error thread tuple type_traits typeindex typeinfo unordered_map unordered_set
  utility valarray variant vector version
  cassert cctype cerrno cfenv cfloat cinttypes climits clocale cmath csetjmp csignal cstdarg
  cstddef cstdint cstdio cstdlib cstring ctime cuchar cwchar cwctype
)

tu="$work/all.cpp"
: > "$tu"
for header in "${headers[@]}"; do
  if echo "#include <$header>" | "$cxx" -std=c++20 -fsyntax-only -x c++ - >/dev/null 2>&1; then
    echo "#include <$header>" >> "$tu"
  else
    echo "skipping <$header> (not available)" >&2
  fi
done

echo "preprocessing $(wc -l < "$tu") headers..." >&2
"$cxx" -std=c++20 -E -P "$tu" -o "$work/all.ii"
echo "preprocessed to $(wc -l < "$work/all.ii") lines" >&2

# The macros those headers define, which the preprocessing above removed:
# `SEEK_SET`, `EOF`, `INT_MAX` and the rest are names a file uses, and a
# name list without them reports every one of them as undefined.
"$cxx" -std=c++20 -E -dM "$tu" | awk '{ print $2 }' | sed 's/(.*//' | sort -u > "$work/macros.txt"
echo "$(wc -l < "$work/macros.txt") macro names" >&2

"$cxx" -std=gnu++20 -O2 -I "$root/src" \
  "$root/scripts/gen_cpp_std_names.cpp" "$root/src/cpp_ast.cpp" "$root/src/cpp_parse.cpp" \
  -o "$work/gen"

LC_ALL=C "$work/gen" "$work/all.ii" "$root/src/cpp_lsp_std_names.cpp" "$work/macros.txt"
