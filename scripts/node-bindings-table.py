#!/usr/bin/env python3
# Regenerates the binding loaders in runtime/node/internal/bootstrap.js from the files in
# runtime/node/internal/bindings (literal requires, so the npm bundler can follow them).
import os, re
root = os.path.join(os.path.dirname(__file__), '..', 'runtime', 'node')
names = sorted(f[:-3] for f in os.listdir(os.path.join(root, 'internal', 'bindings')) if f.endswith('.js'))
p = os.path.join(root, 'internal', 'bootstrap.js')
s = open(p).read()
table = 'const loaders = {\n  __proto__: null,\n' + ''.join(f"  {n}: () => require('internal/bindings/{n}'),\n" for n in names) + '};'
s = re.sub(r'const loaders = \{.*?\n\};', lambda m: table, s, count=1, flags=re.S)
open(p, 'w').write(s)
