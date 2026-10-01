#!/bin/sh
# Copies Node.js lib files into runtime/node, adding the line that gives them `primordials`
# and `internalBinding` (see runtime/node/internal/bootstrap.js). The version is the one
# runtime/node/NODE.md names.
#   scripts/node-import.sh lib/path.js lib/internal/util/inspect.js ...
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
version=v26.3.0
for f in "$@"; do
  # NODE_VERSION=... for a file whose current version uses syntax JavaScriptCore lacks
  version=${NODE_VERSION:-v26.3.0}
  rel=${f#lib/}
  out="$root/runtime/node/$rel"
  mkdir -p "$(dirname "$out")"
  curl -sSfL "https://raw.githubusercontent.com/nodejs/node/$version/lib/$rel" | python3 -c '
import sys
src = sys.stdin.read()
head = "const { primordials, internalBinding } = require(\"internal/bootstrap\");\n"
if "/per_context/" in sys.argv[1]:
    # per-context scripts also see the symbol tables as globals
    head += "const { privateSymbols, perIsolateSymbols } = internalBinding(\"util\");\n"
i = src.index("'"'"'use strict'"'"';")
j = src.index("\n", i) + 1
sys.stdout.write(src[:j] + "\n" + head + src[j:])' "$rel" > "$out"
  echo "$rel"
done
