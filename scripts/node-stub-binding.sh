#!/bin/sh
# Writes runtime/node/internal/bindings/<name>.js as a placeholder: the module loads, and using
# any of its functions throws ERR_NOT_IMPLEMENTED naming it (until the binding is written).
set -eu
root=$(cd "$(dirname "$0")/.." && pwd)
for name in "$@"; do
  f="$root/runtime/node/internal/bindings/$name.js"
  [ -e "$f" ] && continue
  cat > "$f" <<JS
'use strict';

// internalBinding('$name'): not written yet. Loading it works; using it throws.
module.exports = new Proxy({}, {
  get(target, key) {
    if (typeof key === 'symbol' || key === 'then') return undefined;
    return target[key] ??= function notImplemented() {
      const e = new Error(\`internalBinding('$name').\${String(key)} is not implemented in Barm yet\`);
      e.code = 'ERR_NOT_IMPLEMENTED';
      throw e;
    };
  },
});
JS
  echo "stub $name"
done
python3 "$root/scripts/node-bindings-table.py"
