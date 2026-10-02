'use strict';

// internalBinding('timers'): Node.js keeps its timer lists in JS (internal/timers) and asks the
// host for one timer: arm it for `ms`, and when it fires call processTimers(now), which runs what's
// due and returns the next expiry (negative when only unref'd timers remain, 0 for none).
// Immediates run in the loop's check phase: processImmediate. The natives
// (globalThis.__barm_native) behind it:
//   now()                      milliseconds since the loop started
//   timerSetup(onTimer, onCheck)
//   timerSchedule(ms)          (re)arm the timer
//   timerRef(bool)             whether a pending timer keeps the program running
//   requestCheck(ref)          run onCheck in the next loop turn; ref: keep the program running

const native = globalThis.__barm_native ?? require('internal/bootstrap/host_fallback');

const kCount = 0;
const kRefCount = 1;
const kHasOutstanding = 2;

let processImmediate = null;
let processTimers = null;
let checkRequested = false;

// Wires internal/timers to the loop: on the first timer or immediate (whether it came through
// the globals or require('timers')), as Node.js's bootstrap does at startup.
function ensureSetup() {
  if (processTimers !== null) return;
  const queue = require('internal/bootstrap/ticks')();
  ({ processImmediate, processTimers } = require('internal/timers').getTimerCallbacks(queue.runNextTicks));
  const { callFromHost } = require('internal/bindings/task_queue');
  native.timerSetup(() => callFromHost(onTimer), () => callFromHost(onCheck));
}

function requestCheck() {
  ensureSetup();
  if (checkRequested) return;
  checkRequested = true;
  native.requestCheck(immediateFields[kRefCount] > 0);
}

// immediateInfo: adding an immediate (kCount going up) asks for a check phase
const immediateFields = new Uint32Array(3);
const immediateInfo = new Proxy(immediateFields, {
  set(target, key, value) {
    const before = target[key];
    target[key] = value;
    if (Number(key) === kCount && value > before) requestCheck();
    return true;
  },
  get(target, key) {
    const v = target[key];
    return typeof v === 'function' ? v.bind(target) : v;
  },
});

const timeoutInfo = new Int32Array(1);

// The loop's time, as libuv caches it: read once per callback from the loop, not on every timer
// change (outside one, read each time)
let cachedNow = -1;
const resetNow = () => { cachedNow = -1; };
let hostState = null;
function getLibuvNow() {
  hostState ??= require('internal/bindings/task_queue');
  if (!hostState.inHostCallback()) return native.now();
  if (cachedNow < 0) {
    cachedNow = native.now();
    hostState.onHostReturn(resetNow);
  }
  return cachedNow;
}

// (the loop calls them through callFromHost, which runs the tick queue when they return, as
// Node.js does after every callback from the loop, before promise jobs)
function onTimer() {
  const now = native.now();
  const next = processTimers(now);
  if (next === 0) {
    native.timerRef(false);
  } else {
    const due = Math.abs(next) - native.now();
    native.timerSchedule(due > 0 ? due : 1);
    native.timerRef(next > 0);
  }
}

function onCheck() {
  checkRequested = false;
  if (immediateFields[kCount] === 0) return;
  processImmediate();
  if (immediateFields[kCount] > 0) requestCheck();
}

module.exports = {
  immediateInfo,
  timeoutInfo,
  getLibuvNow,
  setupTimers(pi, pt) {
    if (processTimers !== null) return;
    processImmediate = pi;
    processTimers = pt;
    const { callFromHost } = require('internal/bindings/task_queue');
    native.timerSetup(() => callFromHost(onTimer), () => callFromHost(onCheck));
  },
  scheduleTimer(ms) {
    ensureSetup();
    native.timerSchedule(ms > 0 ? ms : 1);
  },
  toggleTimerRef(ref) {
    native.timerRef(ref);
  },
  toggleImmediateRef(ref) {
    if (ref && immediateFields[kCount] > 0) {
      checkRequested = false;
      requestCheck();
    }
  },
  kCount,
  kRefCount,
  kHasOutstanding,
};
