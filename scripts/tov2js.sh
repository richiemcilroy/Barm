#!/bin/sh
# Runs a single-file Tov program under Node (as TypeScript, via type stripping), for reference output.
# Handles the Tov-only syntax the run tests use: `inout` parameters, `&` arguments, `weak` fields,
# `cyclic class`, `throws` clauses, `try f()` markers, `chars()`, `byteLength`.
set -eu
src=$1
work=$(mktemp -d)
trap 'rm -r "$work"' EXIT
python3 - "$src" "$work/prog.ts" <<'PY'
import re, sys
text = open(sys.argv[1]).read()
text = re.sub(r'\binout\s+', '', text)
text = re.sub(r'([(,]\s*)&(?=[A-Za-z_])', r'\1', text)
text = re.sub(r'^(\s*)weak\s+', r'\1', text, flags=re.M)
text = re.sub(r'^cyclic\s+class\b', 'class', text, flags=re.M)
# `throws E` clauses and `try f()` markers (not `try {` blocks).
text = re.sub(r'\)\s*(:\s*[^{=;]+?)?\s+throws\s+[^{;]+?\s*\{', lambda m: ')' + (m.group(1) or '') + ' {', text)
text = re.sub(r'\btry\s+(?!\{)', '', text)
# `throws E` in function types: `(x: T) => R throws E`.
text = re.sub(r'(=>\s*[^\n;,)=]+?)\s+throws\s+[A-Za-z_][\w]*(\s*\|\s*[A-Za-z_][\w]*)*', r'\1', text)

def match_close(t, i, o, c):
    d = 0
    for j in range(i, len(t)):
        if t[j] == o: d += 1
        elif t[j] == c:
            d -= 1
            if d == 0: return j
    return -1

# Parameter properties (not erasable TypeScript): declare the fields at the top of the class,
# assign them at the start of the constructor (after `super(...)`), as tsc does.
pos = 0
while True:
    m = re.search(r'\bconstructor\s*\(', text[pos:])
    if not m: break
    open_p = pos + m.end() - 1
    close_p = match_close(text, open_p, '(', ')')
    params = text[open_p + 1:close_p]
    parts, d, cur = [], 0, ''
    for ch in params:
        if ch in '(<{[': d += 1
        if ch in ')>}]': d -= 1
        if ch == ',' and d == 0: parts.append(cur); cur = ''
        else: cur += ch
    if cur.strip(): parts.append(cur)
    props, new_parts = [], []
    for part in parts:
        mm = re.match(r'(\s*)((?:(?:public|private|protected|readonly)\s+)+)(\w+)', part)
        if mm:
            props.append(mm.group(3))
            part = mm.group(1) + part[mm.end(2):]
        new_parts.append(part)
    if not props:
        pos = close_p
        continue
    new_params = ','.join(new_parts)
    body_open = text.index('{', close_p)
    body_close = match_close(text, body_open, '{', '}')
    body = text[body_open + 1:body_close]
    assigns = ' '.join(f'this.{n} = {n};' for n in props)
    sm = re.search(r'\bsuper\s*\(', body)
    if sm:
        se = match_close(body, sm.end() - 1, '(', ')') + 1
        body = body[:se] + '; ' + assigns + body[se:]
    else:
        body = ' ' + assigns + body
    cls = text.rfind('class ', 0, open_p)
    cls_open = text.index('{', cls)
    decls = ' '.join(f'{n};' for n in props)
    text = (text[:cls_open + 1] + ' ' + decls + text[cls_open + 1:open_p + 1] + new_params + text[close_p:body_open + 1] + body + text[body_close:])
    pos = cls_open + len(decls) + 1

if re.search(r"^(export )?(async )?function main\(", text, flags=re.M):
    text += "\nmain();\n"
open(sys.argv[2], 'w').write(text)
PY
cat > "$work/prelude.mjs" <<'JS'
Object.defineProperty(String.prototype, "chars", { value: function () { return [...this]; } });
Object.defineProperty(String.prototype, "byteLength", { get: function () { return Buffer.byteLength(String(this)); } });
globalThis.test = () => {};
// Tov's keys()/values() return arrays (arrays are iterable, so for...of still works).
const mapKeys = Map.prototype.keys, mapValues = Map.prototype.values, setValues = Set.prototype.values;
Map.prototype.keys = function () { return [...mapKeys.call(this)]; };
Map.prototype.values = function () { return [...mapValues.call(this)]; };
Set.prototype.values = function () { return [...setValues.call(this)]; };
// fetch() errors are Tov FetchErrors (a TypeError with Bun's `code`); Headers.keys()/values()
// return arrays.
globalThis.FetchError ??= TypeError;
if (globalThis.Headers) {
  const hk = Headers.prototype.keys, hv = Headers.prototype.values;
  Headers.prototype.keys = function () { return [...hk.call(this)]; };
  Headers.prototype.values = function () { return [...hv.call(this)]; };
}
JS
# TOV2JS_RUNTIME=bun runs it under Bun (for fetch(), whose reference is Bun's).
if [ "${TOV2JS_RUNTIME:-node}" = bun ]; then
  exec bun --preload "$work/prelude.mjs" "$work/prog.ts"
fi
exec node --no-warnings --import "$work/prelude.mjs" "$work/prog.ts"
