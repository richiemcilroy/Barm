'use strict';

// internalBinding('process_wrap'): child processes (Node.js's src/process_wrap.cc) over Barm's
// natives (runtime/node.c): spawn() starts the program, connecting its pipes to the Pipe handles
// in options.stdio, and onexit(exitCode, signal) runs when it exits.

const native = globalThis.__barm_native?.process;

let runTicks;
function complete(fn) {
  runTicks ??= require('internal/bindings/task_queue').runTicks;
  try {
    fn();
  } finally {
    runTicks();
  }
}

// (runtime/node.c's stdio kinds)
const IGNORE = 0;
const PIPE = 1;
const INHERIT = 2;
const FD = 3;

// a signal number's name ('SIGTERM'), as Node.js's onexit gets it; null for none
let names;
function signalName(n) {
  if (!n) return null;
  if (!names) {
    names = { __proto__: null };
    const signals = require('internal/bindings/constants').os.signals;
    for (const k in signals) names[signals[k]] ??= k;
  }
  return names[n] ?? n;
}

class Process {
  #handle = null;
  #refed = true;

  constructor() {
    this.pid = 0;
    this.onexit = undefined;
  }

  // spawn(options) -> 0 or a UV error
  spawn(options) {
    if (!native) return -38; // UV_ENOSYS
    const specs = [];
    for (const [i, s] of (options.stdio ?? []).entries()) {
      switch (s?.type) {
        case 'pipe':
        case 'overlapped':
          specs.push([PIPE, -1]);
          break;
        case 'inherit':
          specs.push([INHERIT, s.fd ?? i]);
          break;
        case 'fd':
          specs.push([FD, s.fd]);
          break;
        case 'wrap':
          specs.push([FD, s.handle?.fd ?? -1]);
          break;
        default:
          specs.push([IGNORE, -1]);
      }
    }
    const [pid, handle, fds] = native.spawn(options.file, options.args ?? [options.file], options.envPairs ?? null,
      options.cwd ?? null, specs, !!options.detached, (exitCode, signal) => {
        complete(() => this.onexit?.(exitCode, signalName(signal)));
      });
    if (pid < 0) return pid;
    this.pid = pid;
    this.#handle = handle;
    if (!this.#refed) native.ref(handle, false);
    for (const [i, s] of (options.stdio ?? []).entries()) {
      if ((s?.type === 'pipe' || s?.type === 'overlapped') && s.handle && fds[i] >= 0) s.handle._barmOpen(fds[i]);
    }
    return 0;
  }

  kill(signal) {
    return this.#handle ? native.kill(this.#handle, signal) : -3; // UV_ESRCH
  }

  close(callback) {
    if (this.#handle) native.release(this.#handle);
    if (typeof callback === 'function') queueMicrotask(() => callback.call(this));
  }

  ref() {
    this.#refed = true;
    if (this.#handle) native.ref(this.#handle, true);
  }

  unref() {
    this.#refed = false;
    if (this.#handle) native.ref(this.#handle, false);
  }

  hasRef() {
    return this.#refed;
  }

  getAsyncId() {
    return -1;
  }
}

module.exports = { Process };
