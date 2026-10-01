'use strict';

// The `process` global, over Barm's natives (globalThis.__barm_native, which runtime/node.c
// installs). It costs one native call to make: the fields are plain values, and everything that
// needs another module (EventEmitter, nextTick, warnings, stdio) loads it on first use, so a
// package that only reads process.env starts nothing else. The natives this uses:
//   info()          { argv, execArgv, execPath, pid, ppid, platform, arch, env, title }
//   cwd() / chdir(path) / exit(code) / umask(mask)
//   hrtime()        nanoseconds since the program started (a number)
//   write(fd, data) data is a string or a Uint8Array; returns bytes written
//   isatty(fd) / windowSize(fd) -> [columns, rows]
//   memoryUsage()   { rss, heapTotal, heapUsed, external, arrayBuffers, available }
//   cpuUsage()      [user µs, system µs]
//   kill(pid, signal) / ids() -> [uid, gid, euid, egid]

// (outside Barm's runtime, e.g. a bare engine: the host_fallback natives)
const native = globalThis.__barm_native ?? require('internal/bootstrap/host_fallback');
const info = native.info();

const VERSION = 'v26.3.0';

const process = {};
Object.defineProperty(globalThis, 'process', { __proto__: null, value: process, writable: true, enumerable: false, configurable: true });
Object.defineProperty(process, Symbol.toStringTag, { __proto__: null, value: 'process', writable: false, enumerable: false, configurable: true });

function lazyProperty(name, make) {
  Object.defineProperty(process, name, {
    __proto__: null,
    enumerable: true,
    configurable: true,
    get() {
      const value = make();
      Object.defineProperty(process, name, { __proto__: null, value, writable: true, enumerable: true, configurable: true });
      return value;
    },
    set(value) {
      Object.defineProperty(process, name, { __proto__: null, value, writable: true, enumerable: true, configurable: true });
    },
  });
}

process.title = info.title ?? 'barm';
process.version = VERSION;
process.versions = {
  node: VERSION.slice(1),
  barm: info.barmVersion ?? '0.1.0',
  modules: '141',
  uv: '1.51.0',
  boringssl: '0.20260929.0',
  openssl: '3.0.13+quic',
  zlib: '1.3.1',
  brotli: '1.2.0',
  zstd: '1.5.7',
};
process.arch = info.arch;
process.platform = info.platform;
process.release = { name: 'node', lts: undefined };
process.argv = info.argv;
process.execArgv = info.execArgv ?? [];
process.argv0 = info.argv[0] ?? 'barm';
process.execPath = info.execPath;
process.env = info.env;
process.pid = info.pid;
process.ppid = info.ppid;
process.exitCode = undefined;
process.config = { target_defaults: {}, variables: { node_shared_openssl: false, v8_enable_i18n_support: 0 } };
process.features = { inspector: false, debug: false, uv: true, ipv6: true, tls_alpn: true, tls_sni: true, tls_ocsp: false, tls: true, cached_builtins: true, require_module: true, typescript: false };
process.debugPort = 9229;
process.throwDeprecation = false;
process.noDeprecation = false;
process.traceDeprecation = false;
lazyProperty('allowedNodeEnvironmentFlags', () => new Set());

process.cwd = () => native.cwd();
process.chdir = (dir) => {
  require('internal/validators').validateString(dir, 'directory');
  native.chdir(dir);
};
process.umask = (mask) => native.umask(mask === undefined ? -1 : Number(mask));
process.uptime = () => native.hrtime() / 1e9;

function hrtime(previous) {
  const ns = BigInt(Math.round(native.hrtime()));
  let s = Number(ns / 1000000000n);
  let n = Number(ns % 1000000000n);
  if (previous !== undefined) {
    s -= previous[0];
    n -= previous[1];
    if (n < 0) {
      s--;
      n += 1e9;
    }
  }
  return [s, n];
}
hrtime.bigint = () => BigInt(Math.round(native.hrtime()));
process.hrtime = hrtime;

process.memoryUsage = () => native.memoryUsage();
process.memoryUsage.rss = () => native.memoryUsage().rss;
process.cpuUsage = (previous) => {
  const [user, system] = native.cpuUsage();
  return previous ? { user: user - previous.user, system: system - previous.system } : { user, system };
};
process.resourceUsage = () => {
  const [userCPUTime, systemCPUTime] = native.cpuUsage();
  return { userCPUTime, systemCPUTime, maxRSS: Math.round(native.memoryUsage().rss / 1024) };
};
process.constrainedMemory = () => 0;
process.availableMemory = () => native.memoryUsage().available ?? 0;

const ids = () => native.ids();
process.getuid = () => ids()[0];
process.getgid = () => ids()[1];
process.geteuid = () => ids()[2];
process.getegid = () => ids()[3];
process.getgroups = () => [ids()[1]];

process.kill = (pid, signal = 'SIGTERM') => {
  native.kill(pid, signal);
  return true;
};

process.exit = (code) => {
  if (code !== undefined) process.exitCode = code;
  const exitCode = process.exitCode ?? 0;
  if (!process._exiting) {
    process._exiting = true;
    if (emitter) process.emit('exit', exitCode);
  }
  native.exit(process.exitCode ?? exitCode);
};
process.reallyExit = (code) => native.exit(code);
process.abort = () => native.exit(134);

// nextTick: the tick queue starts on first use
process.nextTick = function nextTick(...args) {
  const { nextTick } = require('internal/bootstrap/ticks')();
  process.nextTick = nextTick;
  return nextTick(...args);
};

process.emitWarning = (...args) => require('internal/process/warning').emitWarning(...args);
process.binding = (name) => {
  const e = new Error(`No such module: ${name}`);
  e.code = 'ERR_INVALID_MODULE';
  throw e;
};
process._getActiveHandles = () => [];
process._getActiveRequests = () => [];
process.getActiveResourcesInfo = () => [];
process.setUncaughtExceptionCaptureCallback = (fn) => { process._uncaughtCapture = fn; };
process.hasUncaughtExceptionCaptureCallback = () => typeof process._uncaughtCapture === 'function';
process.setSourceMapsEnabled = () => {};
lazyProperty('report', () => ({ getReport: () => ({}) }));

// EventEmitter: process becomes one (events loads) when first used as one
const emitterMethods = [
  'on', 'once', 'off', 'addListener', 'removeListener', 'removeAllListeners', 'emit', 'listeners',
  'rawListeners', 'listenerCount', 'prependListener', 'prependOnceListener', 'eventNames',
  'setMaxListeners', 'getMaxListeners',
];
let emitter = false;
function becomeEmitter() {
  if (emitter) return;
  emitter = true;
  const EventEmitter = require('events');
  for (const m of emitterMethods) delete process[m];
  Object.setPrototypeOf(process, Object.create(EventEmitter.prototype, {
    [Symbol.toStringTag]: { value: 'process', configurable: true },
  }));
  EventEmitter.init.call(process);
}
for (const m of emitterMethods) {
  Object.defineProperty(process, m, {
    __proto__: null,
    enumerable: false,
    configurable: true,
    writable: true,
    value: function (...args) {
      becomeEmitter();
      return process[m](...args);
    },
  });
}
// uncaught exceptions go to 'uncaughtException' listeners, if there are any
native.setFatalHandler?.((err) => {
  if (!emitter || process.listenerCount('uncaughtException') === 0) return false;
  process.emit('uncaughtException', err, 'uncaughtException');
  return true;
});

// for the host: whether anything can be listening (to 'exit' and 'beforeExit')
Object.defineProperty(process, '_barmEmitter', { __proto__: null, get: () => emitter, enumerable: false, configurable: true });

// stdio: synchronous writes to fds 1 and 2 (as Node.js does for files, and pipes on macOS)
function stdioStream(fd) {
  if (native.isatty(fd)) {
    const { WriteStream } = require('tty');
    return new WriteStream(fd);
  }
  const { Writable } = require('stream');
  const stream = new Writable({
    write(chunk, encoding, cb) {
      native.write(fd, typeof chunk === 'string' ? chunk : new Uint8Array(chunk.buffer, chunk.byteOffset, chunk.byteLength));
      cb();
    },
    decodeStrings: false,
  });
  stream.fd = fd;
  stream._type = 'fs';
  stream._isStdio = true;
  stream.destroySoon = stream.destroy;
  stream._destroy = (err, cb) => cb(err);
  return stream;
}
lazyProperty('stdout', () => stdioStream(1));
lazyProperty('stderr', () => stdioStream(2));
lazyProperty('stdin', () => (native.stdin ? native.stdin() : new (require('stream').Readable)({ read() { this.push(null); } })));

module.exports = process;
