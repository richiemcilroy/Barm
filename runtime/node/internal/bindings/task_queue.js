'use strict';

// internalBinding('task_queue'): microtasks, and the hook that runs process.nextTick's queue.
//
// In Node.js, C++ calls the tick callback (processTicksAndRejections) when a callback into JS
// returns, so ticks run before that turn's promise jobs. Here, scheduling a tick (setting
// tickInfo[kHasTickScheduled]) queues one run of the callback as a microtask. A Barm program's
// event loop will also run it right after each callback it makes into JS, before the engine
// drains promise jobs, which gives Node.js's order.

const kHasTickScheduled = 0;
const kHasRejectionToWarn = 1;

// (promise jobs: JavaScriptCore has no queueMicrotask of its own, and globalThis.queueMicrotask
// is Node.js's, which is built on this)
const resolved = Promise.resolve();
const enqueue = (fn) => { resolved.then(fn); };

let tickCallback = null;
let drainQueued = false;
const fields = new Int32Array(2);

function drain() {
  drainQueued = false;
  if (tickCallback !== null && fields[kHasTickScheduled] === 1) tickCallback();
}

const tickInfo = new Proxy(fields, {
  set(target, key, value) {
    target[key] = value;
    if (Number(key) === kHasTickScheduled && value === 1 && !drainQueued) {
      drainQueued = true;
      enqueue(drain);
    }
    return true;
  },
  get(target, key) {
    const v = target[key];
    return typeof v === 'function' ? v.bind(target) : v;
  },
});

let promiseRejectCallback = null;

module.exports = {
  tickInfo,
  enqueueMicrotask: enqueue,
  setTickCallback(fn) {
    tickCallback = fn;
  },
  // promise jobs can't be run on demand from JS: the engine runs them when this turn ends
  runMicrotasks() {},
  promiseRejectEvents: {
    kPromiseRejectWithNoHandler: 0,
    kPromiseHandlerAddedAfterReject: 1,
    kPromiseResolveAfterResolved: 2,
    kPromiseRejectAfterResolved: 3,
  },
  setPromiseRejectCallback(fn) {
    promiseRejectCallback = fn;
  },
  // for the host: the callback to report unhandled rejections to
  getPromiseRejectCallback: () => promiseRejectCallback,
  // for the host: run pending ticks now (after a callback into JS returns)
  runTicks: drain,
  kHasTickScheduled,
  kHasRejectionToWarn,
};
