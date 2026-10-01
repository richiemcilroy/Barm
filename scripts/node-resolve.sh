#!/bin/sh
# Runs a shim test repeatedly, importing each Node.js lib file it finds missing (and stubbing
# missing bindings), until it stops asking for files.
#   scripts/node-resolve.sh <test-name>
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
for i in $(seq 1 60); do
  "$root/scripts/node-shims.sh" "$1" > /dev/null 2>&1 && break
  miss=$(grep -o "Could not open file: [^ ]*runtime/node/[^ ]*\.js" /tmp/node-shims-got.txt | head -1 | sed 's|.*runtime/node/||')
  if [ -z "$miss" ]; then
    b=$(grep -o "No such binding: [a-z_0-9]*" /tmp/node-shims-got.txt | head -1 | sed 's/No such binding: //')
    [ -n "$b" ] && miss="internal/bindings/$b.js"
  fi
  [ -z "$miss" ] && break
  case $miss in
    internal/bindings/*) b=${miss#internal/bindings/}; "$root/scripts/node-stub-binding.sh" "${b%.js}" ;;
    *) echo "import $miss"; "$root/scripts/node-import.sh" "lib/$miss" > /dev/null 2>&1 || { echo "not in Node.js lib: $miss"; break; } ;;
  esac
done
"$root/scripts/node-shims.sh" "$1"
