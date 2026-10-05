'use strict';

// internalBinding('task_queue'): microtasks, and the hook that runs process.nextTick's queue.
//
// In Node.js, C++ calls the tick callback (processTicksAndRejections) when a callback into JS
// returns, so ticks run before that turn's promise jobs. Here every callback from Tov's loop
// goes through callFromHost(), which runs the tick queue when the callback returns, in the same
// call. A tick scheduled outside one (in a promise job, say) queues one run of the queue as a
// promise job instead. internal/process/task_queues tells this binding when a tick is scheduled
// (setTickScheduled).

const kHasTickScheduled = 0;
const kHasRejectionToWarn = 1;

// (promise jobs: JavaScriptCore has no queueMicrotask of its own, and globalThis.queueMicrotask
// is Node.js's, which is built on this)
const resolved = Promise.resolve();
const enqueue = (fn) => { resolved.then(fn); };

let tickCallback = null;
let drainQueued = false;
let hostDepth = 0;
const tickInfo = new Int32Array(2);

function drain() {
  drainQueued = false;
  if (tickCallback === null || tickInfo[kHasTickScheduled] !== 1) return;
  try {
    tickCallback();
  } catch (e) {
    // (in a promise job, a throw would be lost: it's an uncaught exception, as in Node.js)
    const fatal = globalThis.__tov_native?.fatal;
    if (!fatal) throw e;
    fatal(e);
  }
}

// a tick was scheduled: the queue runs when the host's callback returns, else as a promise job
function setTickScheduled() {
  if (hostDepth === 0 && !drainQueued) {
    drainQueued = true;
    enqueue(drain);
  }
}

// what runs when the host's callback returns (once each)
let onReturn = null;

// a callback from the host (Tov's loop): fn(...args), then the tick queue
function callFromHost(fn, a, b, c, d, e, f) {
  hostDepth++;
  try {
    return fn(a, b, c, d, e, f);
  } finally {
    // (the queue runs while still in the callback: ticks it schedules join it, not a promise job)
    if (hostDepth === 1) {
      if (tickInfo[kHasTickScheduled] === 1) drain();
      if (onReturn !== null) {
        const ret = onReturn;
        onReturn = null;
        ret();
      }
    }
    hostDepth--;
  }
}

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
  setTickScheduled,
  callFromHost,
  inHostCallback: () => hostDepth > 0,
  // (one at a time: the timers' cached loop time)
  onHostReturn(fn) {
    onReturn = fn;
  },
  kHasTickScheduled,
  kHasRejectionToWarn,
};
