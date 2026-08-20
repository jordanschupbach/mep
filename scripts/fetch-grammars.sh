#!/usr/bin/env bash
# Builds every dynamically-loaded Treesitter grammar mep knows a highlight
# query for (scripts/ts_grammars.tsv, kept in step with src/treesitter.cpp's
# own DynamicLanguageTable) into $OUT_DIR/<canonical_name>.so -- the same
# `.so`-per-canonical-name layout `LoadDynamicLanguage`
# (src/treesitter.cpp) already searches $MEP_TS_PARSER_PATH for, so this
# is a Nix-free way to get the same "org-babel src block, and any other
# file, gets real syntax highlighting" result flake.nix's
# `pkgs.tree-sitter.withPlugins` devShell already gives Nix users --
# useful on a machine without Nix, or to get a grammar not in that list.
#
# No tree-sitter CLI dependency: every grammar listed here ships a
# committed, already-generated src/parser.c (that's *why* each one is on
# this particular list -- see third_party/README.md's own note on
# grammars that don't and are skipped entirely rather than pulling in a
# CLI+Node/Rust toolchain just to generate one). Building one down to a
# .so is exactly the two-or-three-file compile CMakeLists.txt's own
# `ts_build` function already does for the compiled-in core set, just
# with `cc`/`c++ -shared -fPIC` instead of a static archive.
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
TABLE="$SCRIPT_DIR/ts_grammars.tsv"
OUT_DIR="${MEP_GRAMMAR_OUT_DIR:-$SCRIPT_DIR/../.ts-grammars}"
SRC_DIR="$OUT_DIR/src"
LIB_DIR="$OUT_DIR/lib"
FORCE="${1:-}"

mkdir -p "$SRC_DIR" "$LIB_DIR"

CC="${CC:-cc}"
CXX="${CXX:-c++}"

built=()
skipped=()
failed=()

# Downloads+extracts `repo`@`rev` into $SRC_DIR/<repo-with-slash-as-_>@<rev>
# once, reused by every row referencing the same (repo, rev) pair (e.g.
# typescript/tsx, ocaml/ocaml_interface both come from one tarball).
fetch_repo() {
  local repo="$1" rev="$2"
  local key="${repo//\//_}@${rev}"
  local dest="$SRC_DIR/$key"
  if [ -d "$dest" ]; then
    echo "$dest"
    return 0
  fi
  local tmp
  tmp="$(mktemp -d)"
  if ! curl -sL --fail --max-time 60 "https://github.com/$repo/archive/$rev.tar.gz" \
      | tar xz --strip-components=1 -C "$tmp" 2>/dev/null; then
    rm -rf "$tmp"
    return 1
  fi
  mv "$tmp" "$dest"
  echo "$dest"
}

# Compiles one grammar's `src/` (parser.c + optional scanner.c/scanner.cc)
# into $LIB_DIR/<name>.so.
build_one() {
  local name="$1" grammar_src="$2"
  local src="$grammar_src/src"
  if [ ! -f "$src/parser.c" ]; then
    echo "  ! $name: no src/parser.c in $grammar_src (bad subdir in ts_grammars.tsv?)" >&2
    return 1
  fi
  local work
  work="$(mktemp -d)"
  local objs=()
  "$CC" -fPIC -c -I"$src" "$src/parser.c" -o "$work/parser.o" || { rm -rf "$work"; return 1; }
  objs+=("$work/parser.o")
  local linker="$CC"
  if [ -f "$src/scanner.c" ]; then
    "$CC" -fPIC -c -I"$src" "$src/scanner.c" -o "$work/scanner.o" || { rm -rf "$work"; return 1; }
    objs+=("$work/scanner.o")
  elif [ -f "$src/scanner.cc" ]; then
    "$CXX" -fPIC -c -std=c++17 -I"$src" "$src/scanner.cc" -o "$work/scanner.o" || { rm -rf "$work"; return 1; }
    objs+=("$work/scanner.o")
    linker="$CXX"
  fi
  "$linker" -shared -o "$LIB_DIR/$name.so" "${objs[@]}" || { rm -rf "$work"; return 1; }
  rm -rf "$work"
  return 0
}

total=0
while IFS=$'\t' read -r name repo rev subdir; do
  [ -z "$name" ] && continue
  case "$name" in \#*) continue ;; esac
  total=$((total + 1))

  if [ -f "$LIB_DIR/$name.so" ] && [ "$FORCE" != "--force" ]; then
    skipped+=("$name")
    continue
  fi

  printf 'building %-16s (%s@%s)\n' "$name" "$repo" "${rev:0:12}"
  if ! grammar_src="$(fetch_repo "$repo" "$rev")"; then
    echo "  ! $name: failed to fetch $repo@$rev" >&2
    failed+=("$name")
    continue
  fi
  if [ -n "${subdir:-}" ]; then
    grammar_src="$grammar_src/$subdir"
  fi
  if build_one "$name" "$grammar_src"; then
    built+=("$name")
  else
    echo "  ! $name: build failed" >&2
    failed+=("$name")
  fi
done < <(grep -v '^#' "$TABLE" | grep -v '^[[:space:]]*$')

echo
echo "grammars: $total total, ${#built[@]} built, ${#skipped[@]} already present, ${#failed[@]} failed"
if [ "${#failed[@]}" -gt 0 ]; then
  echo "failed: ${failed[*]}"
fi
echo "output: $LIB_DIR"
echo
echo "Add it to MEP_TS_PARSER_PATH (highest-priority search location --"
echo "see src/treesitter.cpp's DynamicSearchPaths) to actually use these:"
echo "  export MEP_TS_PARSER_PATH=\"$LIB_DIR\${MEP_TS_PARSER_PATH:+:\$MEP_TS_PARSER_PATH}\""
echo "(the 'run' justfile recipe already does this automatically when"
echo "$LIB_DIR exists)"
