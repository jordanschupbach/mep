#!/bin/sh
# Dependency audit (plans/DEPENDENCY_REMOVAL_PLAN.md, Phase 0): lists the
# libraries the mep binary links and the programs mep's own code runs, and
# fails on anything scripts/deps-allowlist.txt does not account for.
#
#   scripts/deps-audit.sh [path/to/mep]     (default build/native/mep)
#
# The program scan is a heuristic over src/: argv literals handed to
# job_start/RunProcess/Spawn/posix_spawnp/execvp (and their wrappers), and
# Lua/C++ tables that look like an argv (a word followed by a '-flag').
# Tests, smoke drivers and benchmarks are skipped.
set -eu
cd "$(dirname "$0")/.."
bin=${1:-build/native/mep}
allow=scripts/deps-allowlist.txt
status=0

# kind_of TYPE NAME -> prints the kind of the first matching allowlist line, or nothing.
kind_of() {
    while read -r type pattern kind _rest; do
        case "$type" in lib|prog) ;; *) continue ;; esac
        [ "$type" = "$1" ] || continue
        # shellcheck disable=SC2254
        case "$2" in $pattern) echo "$kind"; return ;; esac
    done < "$allow"
}

report() {  # report TYPE NAME
    kind=$(kind_of "$1" "$2")
    if [ -z "$kind" ]; then
        printf '  %-8s %s\n' "UNKNOWN" "$2"
        status=1
    elif [ "$kind" != os ]; then
        printf '  %-8s %s\n' "$kind" "$2"
    fi
}

echo "Linked libraries ($bin):"
if [ ! -f "$bin" ]; then
    echo "  (not built: skipped)"
else
    case "$(uname -s)" in
        Darwin) libs=$(otool -L "$bin" | tail -n +2 | awk '{print $1}') ;;
        *) libs=$(ldd "$bin" | awk '{print $1}') ;;
    esac
    for l in $libs; do report lib "$l"; done
fi

echo "Programs mep's own code runs:"
progs=$(find src -name '*.cpp' -o -name '*.mm' -o -name '*.h' |
    grep -v -e '_test\.cpp$' -e '_smoke_main\.cpp$' -e 'bench' |
    xargs grep -hoE "(job_start|_run|Spawn|SpawnTask|RunProcess|_action|_cmd)\(\s*\{\s*['\"][a-zA-Z0-9_.+/-]+['\"]|\{\s*'[a-z][a-zA-Z0-9_.+-]*'\s*,\s*'-|\{\s*\"[a-z][a-zA-Z0-9_.+-]*\"\s*,\s*\"-|(posix_spawnp|execvp)\([^,\"]*,?\s*\"[a-z][^\"]*\"" |
    grep -oE "['\"][a-zA-Z0-9_.+/-]+['\"]" | tr -d "'\"" | sort -u)
for p in $progs; do report prog "$p"; done

if [ "$status" -ne 0 ]; then
    echo "deps-audit: UNKNOWN entries above -- replace them, or classify them in $allow"
else
    echo "deps-audit: ok (os entries not listed)"
fi
exit "$status"
