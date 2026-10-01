#!/bin/sh
# Checks runtime/node's shims: each tests/node/<name>.js runs under Node.js (its own built-ins)
# and under a bare JavaScriptCore with the shims behind require(); the outputs must match.
#   scripts/node-shims.sh [name...]
set -u
root=$(cd "$(dirname "$0")/.." && pwd)
jsc=/System/Library/Frameworks/JavaScriptCore.framework/Versions/Current/Helpers/jsc
platform=$(node -p process.platform)
fail=0
names=${*:-$(cd "$root/tests/node" && ls *.js | grep -v '^harness\.js$' | sed 's/\.js$//')}
for name in $names; do
  t="$root/tests/node/$name.js"
  # stdout only: Node.js's warnings (stderr) carry its pid
  want=$(cd "$root" && node "$t" 2>/dev/null)
  got=$(cd "$root" && "$jsc" "$root/tests/node/harness.js" -- "$root/runtime/node" "$t" "$root" "$platform" 2>&1)
  if [ "$want" = "$got" ]; then
    echo "ok   $name"
  else
    echo "FAIL $name"
    printf '%s\n' "$want" > /tmp/node-shims-want.txt
    printf '%s\n' "$got" > /tmp/node-shims-got.txt
    diff /tmp/node-shims-want.txt /tmp/node-shims-got.txt | head -20
    fail=1
  fi
done
exit $fail
