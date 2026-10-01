'use strict';

// internalBinding('spawn_sync'): Node.js's src/spawn_sync.cc over Barm's native spawnSync
// (runtime/node.c): runs a program to the end, writing each pipe's input and collecting its
// output, with a timeout and a size limit, and answers as Node.js's does.

const native = globalThis.__barm_native;

// (runtime/node.c's stdio kinds)
const IGNORE = 0;
const PIPE = 1;
const INHERIT = 2;
const FD = 3;

function signalNumber(signal) {
  if (typeof signal === 'number') return signal;
  return require('internal/bindings/constants').os.signals[signal] ?? 15;
}

// spawn(options) -> { pid, status, signal, output, error }
function spawn(options) {
  if (!native?.spawnSync) {
    const e = new Error('child processes are not available outside Barm\'s runtime');
    e.code = 'ERR_METHOD_NOT_IMPLEMENTED';
    throw e;
  }
  const stdio = [];
  const inputs = [];
  for (const [i, s] of (options.stdio ?? []).entries()) {
    switch (s?.type) {
      case 'pipe':
      case 'overlapped':
        stdio.push([PIPE, -1]);
        break;
      case 'inherit':
        stdio.push([INHERIT, s.fd ?? i]);
        break;
      case 'fd':
        stdio.push([FD, s.fd]);
        break;
      default:
        stdio.push([IGNORE, -1]);
    }
    inputs.push(s?.input ?? null);
  }
  const [pid, status, signal, error, output] = native.spawnSync(
    options.file,
    options.args ?? [options.file],
    options.envPairs ?? null,
    options.cwd ?? null,
    stdio,
    inputs,
    options.timeout > 0 ? options.timeout : 0,
    options.maxBuffer ?? 1024 * 1024,
    signalNumber(options.killSignal ?? 'SIGTERM'),
    !!options.detached,
  );
  const result = { status, signal, output: null, pid };
  if (output) {
    const { Buffer } = require('buffer');
    result.output = output.map((o) => (o === null ? null : Buffer.from(o.buffer, o.byteOffset, o.byteLength)));
  }
  if (error) result.error = -error;
  return result;
}

module.exports = { spawn };
