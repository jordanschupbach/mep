#!/usr/bin/env bash
# A small bash tour: functions, arrays, arithmetic, and a here-string.
set -euo pipefail

distance() {
  # bash has integer arithmetic only, so hand the float math to awk.
  awk -v x1="$1" -v y1="$2" -v x2="$3" -v y2="$4" \
    'BEGIN { printf "%.2f\n", sqrt((x1 - x2) ^ 2 + (y1 - y2) ^ 2) }'
}

sum() {
  local total=0
  for n in "$@"; do
    total=$((total + n))
  done
  echo "$total"
}

echo "hello, world"
echo "distance = $(distance 0 0 3 4)"

nums=(1 2 3 4 5)
echo "sum = $(sum "${nums[@]}")"

squares=()
for n in "${nums[@]}"; do
  squares+=($((n * n)))
done
echo "squares = ${squares[*]}"

declare -A by_letter=()
while read -r word; do
  by_letter[${word:0:1}]=$(( ${by_letter[${word:0:1}]:-0} + 1 ))
done <<< $'apple\nbanana\navocado\ncherry'
for k in $(printf '%s\n' "${!by_letter[@]}" | sort); do
  echo "$k: ${by_letter[$k]}"
done
