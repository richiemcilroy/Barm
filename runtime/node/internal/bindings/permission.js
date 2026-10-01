'use strict';

// internalBinding('permission'): not written yet. Loading it works; using it throws.
module.exports = new Proxy({}, {
  get(target, key) {
    if (typeof key === 'symbol' || key === 'then') return undefined;
    return target[key] ??= function notImplemented() {
      const e = new Error(`internalBinding('permission').${String(key)} is not implemented in Barm yet`);
      e.code = 'ERR_NOT_IMPLEMENTED';
      throw e;
    };
  },
});
