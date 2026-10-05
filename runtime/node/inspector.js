'use strict';

// inspector (Tov's own): Tov programs have no V8 inspector to connect to. The module loads, a
// Session can be made, and connecting or posting to one fails as with Node.js built without the
// inspector (ERR_INSPECTOR_NOT_AVAILABLE).

const EventEmitter = require('events');

function notAvailable() {
  const e = new Error('Inspector is not available');
  e.code = 'ERR_INSPECTOR_NOT_AVAILABLE';
  return e;
}

class Session extends EventEmitter {
  connect() {
    throw notAvailable();
  }

  connectToMainThread() {
    throw notAvailable();
  }

  post(method, params, callback) {
    if (typeof params === 'function') callback = params;
    const e = notAvailable();
    if (typeof callback === 'function') {
      process.nextTick(callback, e);
      return;
    }
    throw e;
  }

  disconnect() {}
}

const noop = () => {};
const domain = new Proxy({}, { get: () => noop });

module.exports = {
  open() {
    throw notAvailable();
  },
  close() {},
  url: () => undefined,
  waitForDebugger() {
    throw notAvailable();
  },
  console: globalThis.console,
  Session,
  Network: domain,
  NetworkResources: domain,
  DOMStorage: domain,
};
