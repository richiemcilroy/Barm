#!/bin/sh
# Imports every Node.js lib file that a file in runtime/node requires (statically) and that
# isn't there yet, repeatedly, until nothing is missing. (internal/bindings/* are Tov's own:
# missing ones get placeholders.)
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
for round in $(seq 1 20); do
  missing=$(cd "$root/runtime/node" && find . -name '*.js' -exec grep -oh "require('[^']*')" {} + | sed "s/require('\([^']*\)')/\1/" | sed 's/^node://' | sort -u | while read -r m; do [ -f "$m.js" ] || echo "$m"; done)
  [ -z "$missing" ] && break
  for m in $missing; do
    case $m in
      internal/bindings/*) "$root/scripts/node-stub-binding.sh" "${m#internal/bindings/}" ;;
      *) "$root/scripts/node-import.sh" "lib/$m.js" > /dev/null 2>&1 && echo "import $m" || echo "not in Node.js lib: $m" ;;
    esac
  done
done
