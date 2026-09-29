#!/bin/sh
# Runs a single-file Barm program under Node (as TypeScript, via type stripping), for reference output.
# Handles the Barm-only syntax the run tests use: `inout` parameters, `&` arguments, `chars()`, `byteLength`.
set -eu
src=$1
work=$(mktemp -d)
trap 'rm -r "$work"' EXIT
python3 - "$src" "$work/prog.ts" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
text = re.sub(r'\binout\s+', '', text)
text = re.sub(r'([(,]\s*)&(?=[A-Za-z_])', r'\1', text)
text += "\nmain();\n"
open(sys.argv[2], 'w').write(text)
PY
cat > "$work/prelude.mjs" <<'JS'
Object.defineProperty(String.prototype, "chars", { value: function () { return [...this]; } });
Object.defineProperty(String.prototype, "byteLength", { get: function () { return Buffer.byteLength(String(this)); } });
globalThis.test = () => {};
// Barm's keys()/values() return arrays (arrays are iterable, so for...of still works).
const mapKeys = Map.prototype.keys, mapValues = Map.prototype.values, setValues = Set.prototype.values;
Map.prototype.keys = function () { return [...mapKeys.call(this)]; };
Map.prototype.values = function () { return [...mapValues.call(this)]; };
Set.prototype.values = function () { return [...setValues.call(this)]; };
JS
exec node --no-warnings --import "$work/prelude.mjs" "$work/prog.ts"
