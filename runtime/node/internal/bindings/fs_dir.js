'use strict';

// internalBinding('fs_dir'): not written yet. Loading it works; using it throws.
module.exports = new Proxy({}, {
  get(target, key) {
    if (typeof key === 'symbol' || key === 'then') return undefined;
    return target[key] ??= function notImplemented() {
      const e = new Error(`internalBinding('fs_dir').${String(key)} is not implemented in Tov yet`);
      e.code = 'ERR_NOT_IMPLEMENTED';
      throw e;
    };
  },
});
