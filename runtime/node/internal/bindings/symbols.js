'use strict';

// internalBinding('symbols'): Node.js's per-isolate symbols (oninit, owner_symbol, ...), made
// on first use.
module.exports = new Proxy({ __proto__: null }, {
  get(cache, name) {
    if (typeof name !== 'string') return undefined;
    return cache[name] ??= Symbol(name);
  },
});
