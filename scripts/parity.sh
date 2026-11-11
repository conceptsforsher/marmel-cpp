#!/usr/bin/env bash
# LOC parity report for the commit-by-commit marmel -> C++ translation.
#
# Usage: bash scripts/parity.sh <upstream-clone-dir> <map-file>
#
# <upstream-clone-dir> is a clone of https://github.com/Na1w/marmel
# <map-file>          lines of the form "<folder> <upstream-sha>"
#
# Prints a markdown table (upstream Rust LOC per commit vs translated C++ LOC)
# and a coverage summary: how many of the upstream commits are translated.
set -euo pipefail

UP="${1:-upstream}"
MAP="${2:-parity-map.txt}"

loc_tree() { # $1 = dir, rest = find args
  local dir="$1"; shift
  local files
  files=$(find "$dir" -type f \( "$@" \) -not -path '*/target/*' 2>/dev/null || true)
  if [ -z "$files" ]; then echo 0; else printf '%s\n' "$files" | xargs cat | wc -l; fi
}

upstream_loc() { # $1 = sha
  git -C "$UP" checkout -q "$1"
  loc_tree "$UP" -name '*.rs'
}

cpp_loc() { # $1 = folder
  loc_tree "$1" -name '*.cpp' -o -name '*.cc' -o -name '*.h' -o -name '*.hpp'
}

total_commits=$(git -C "$UP" rev-list --count HEAD)
head_loc=$(upstream_loc HEAD)

echo "# LOC parity report"
echo
echo "- upstream HEAD: \`$(git -C "$UP" rev-parse --short HEAD)\`"
echo "- upstream commits total: **$total_commits**"
echo "- upstream Rust LOC at HEAD: **$head_loc**"
echo
echo "| commit folder | upstream sha | upstream Rust LOC | C++ LOC | C++/Rust |"
echo "|---|---|---:|---:|---:|"

mapped=0
up_sum=0
cpp_sum=0
while read -r folder sha; do
  [ -z "${folder:-}" ] && continue
  case "$folder" in \#*) continue ;; esac
  mapped=$((mapped + 1))
  u=$(upstream_loc "$sha")
  c=$(cpp_loc "$folder")
  up_sum=$((up_sum + u))
  cpp_sum=$((cpp_sum + c))
  if [ "$u" -gt 0 ]; then
    ratio=$(awk -v a="$c" -v b="$u" 'BEGIN { printf "%.2f", a / b }')
  else
    ratio="n/a"
  fi
  echo "| \`$folder\` | \`$sha\` | $u | $c | $ratio |"
done < "$MAP"

echo
echo "| | commits | Rust LOC | C++ LOC | C++/Rust |"
echo "|---|---:|---:|---:|---:|"
total_ratio="n/a"
if [ "$up_sum" -gt 0 ]; then
  total_ratio=$(awk -v a="$cpp_sum" -v b="$up_sum" 'BEGIN { printf "%.2f", a / b }')
fi
echo "| translated so far | $mapped / $total_commits | $up_sum | $cpp_sum | $total_ratio |"
echo
echo "- Untranslated upstream commits: **$((total_commits - mapped))**"
echo
echo "Note: \`upstream Rust LOC\` counts every \`*.rs\` file in the tree at that"
echo "upstream commit (src + tests + crates); \`C++ LOC\` counts every"
echo "\`*.cpp/*.h/*.hpp\` in the translation folder. prompts/*.md and config"
echo "files are carried verbatim and are not counted on either side."
