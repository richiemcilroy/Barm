'use strict';

// Node.js's globals, installed before an npm bundle's first module runs (the bundler loads this
// as shim "__globals"). Each one is a getter that loads its module on first use, so a program
// pays only for the globals it touches; assigning to one replaces it, as in Node.js. (Every
// require here is a literal, so the bundler can include what it names.)

const g = globalThis;

// what a global's loader returns when the bundle doesn't hold its module: the global goes away
const ABSENT = Symbol('absent');

// (Node.js's versions replace the engine's or host's, where there are any)
function lazy(name, load, enumerable = false) {
  let value;
  let loaded = false;
  Object.defineProperty(g, name, {
    __proto__: null,
    enumerable,
    configurable: true,
    get() {
      if (!loaded) {
        value = load();
        if (value === ABSENT) {
          delete g[name];
          return undefined;
        }
        loaded = true;
      }
      return value;
    },
    set(v) {
      Object.defineProperty(g, name, { __proto__: null, value: v, writable: true, enumerable, configurable: true });
    },
  });
}


Object.defineProperty(g, 'global', { __proto__: null, value: g, writable: true, enumerable: false, configurable: true });
// (as Bun has it: the global object under its web name)
Object.defineProperty(g, 'self', { __proto__: null, value: g, writable: true, enumerable: false, configurable: true });

// V8's Error.prepareStackTrace and CallSites (internal/barm/callsite), for packages that read
// their callers: while it's a function, Error.captureStackTrace and `new Error()` (the global
// Error is then a wrapper around the engine's) give it the error and its frames.
{
  const NativeError = g.Error;
  const nativeCapture = NativeError.captureStackTrace;
  let prepareStackTrace;
  // (its own frame, if the engine keeps it, isn't one of the error's)
  const PreparingError = function Error(...args) {
    const e = Reflect.construct(NativeError, args, new.target ?? PreparingError);
    if (typeof prepareStackTrace === 'function') {
      const stack = require('internal/barm/callsite').prepare(e, e.stack, /^Error@/.test(e.stack) ? 1 : 0);
      Object.defineProperty(e, 'stack', { __proto__: null, value: stack, writable: true, configurable: true });
    }
    return e;
  };
  PreparingError.prototype = NativeError.prototype;
  Object.setPrototypeOf(PreparingError, NativeError);
  for (const target of [NativeError, PreparingError]) {
    Object.defineProperty(target, 'prepareStackTrace', {
      __proto__: null,
      configurable: true,
      enumerable: false,
      get: () => prepareStackTrace,
      set(fn) {
        prepareStackTrace = fn;
        if (typeof fn === 'function') {
          if (g.Error === NativeError) g.Error = PreparingError;
        } else if (g.Error === PreparingError) {
          g.Error = NativeError;
        }
      },
    });
  }
  Object.defineProperty(PreparingError, 'stackTraceLimit', {
    __proto__: null,
    configurable: true,
    enumerable: true,
    get: () => NativeError.stackTraceLimit,
    set(n) { NativeError.stackTraceLimit = n; },
  });
  if (typeof nativeCapture === 'function') {
    const captureStackTrace = function captureStackTrace(obj, fn) {
      nativeCapture(obj, fn ?? captureStackTrace);
      if (typeof prepareStackTrace === 'function') {
        const stack = require('internal/barm/callsite').prepare(obj, obj.stack, 0);
        Object.defineProperty(obj, 'stack', { __proto__: null, value: stack, writable: true, configurable: true });
      }
    };
    Object.defineProperty(NativeError, 'captureStackTrace', { __proto__: null, value: captureStackTrace, writable: true, configurable: true });
  }
}

// process: a plain object (it sets the global itself), with the rest of Node.js's process
// loading as it's used
require('internal/bootstrap/process');

// (bound to process.stdout and process.stderr on first write, as Node.js's startup does)
lazy('console', () => {
  const console = require('internal/console/global');
  require('internal/console/constructor').initializeGlobalConsole(console);
  return console;
});
// the global `crypto` (Web Crypto): random values without the rest of crypto; `subtle` brings
// crypto in when the program's bundle has it
lazy('crypto', () => {
  const { getRandomValues, randomUUID } = require('internal/barm/random');
  const webcrypto = { getRandomValues, randomUUID };
  Object.defineProperty(webcrypto, 'subtle', {
    __proto__: null,
    enumerable: true,
    configurable: true,
    get() {
      const id = 'crypto';
      return bundled(id)?.subtle;
    },
  });
  return webcrypto;
});
lazy('Buffer', () => require('buffer').Buffer);
lazy('atob', () => require('buffer').atob);
lazy('btoa', () => require('buffer').btoa);
lazy('queueMicrotask', () => require('internal/process/task_queues').queueMicrotask);
lazy('structuredClone', () => require('internal/bindings/messaging').structuredClone);

lazy('TextEncoder', () => require('internal/encoding').TextEncoder);
lazy('TextDecoder', () => require('internal/encoding').TextDecoder);
lazy('URL', () => require('internal/url').URL);
lazy('URLSearchParams', () => require('internal/url').URLSearchParams);
lazy('AbortController', () => require('internal/abort_controller').AbortController);
lazy('AbortSignal', () => require('internal/abort_controller').AbortSignal);
lazy('Event', () => require('internal/event_target').Event);
lazy('EventTarget', () => require('internal/event_target').EventTarget);
lazy('CustomEvent', () => require('internal/event_target').CustomEvent);
lazy('DOMException', () => require('internal/bindings/messaging').DOMException);
lazy('MessageChannel', () => require('internal/worker/io').MessageChannel);
lazy('MessagePort', () => require('internal/worker/io').MessagePort);
lazy('BroadcastChannel', () => require('internal/worker/io').BroadcastChannel);
lazy('MessageEvent', () => require('internal/deps/undici/undici').MessageEvent);
lazy('ReadableStream', () => require('internal/webstreams/readablestream').ReadableStream);
lazy('ReadableStreamDefaultReader', () => require('internal/webstreams/readablestream').ReadableStreamDefaultReader);
lazy('ReadableStreamBYOBReader', () => require('internal/webstreams/readablestream').ReadableStreamBYOBReader);
lazy('ReadableStreamBYOBRequest', () => require('internal/webstreams/readablestream').ReadableStreamBYOBRequest);
lazy('ReadableByteStreamController', () => require('internal/webstreams/readablestream').ReadableByteStreamController);
lazy('ReadableStreamDefaultController', () => require('internal/webstreams/readablestream').ReadableStreamDefaultController);
lazy('WritableStream', () => require('internal/webstreams/writablestream').WritableStream);
lazy('WritableStreamDefaultController', () => require('internal/webstreams/writablestream').WritableStreamDefaultController);
lazy('WritableStreamDefaultWriter', () => require('internal/webstreams/writablestream').WritableStreamDefaultWriter);
lazy('TransformStream', () => require('internal/webstreams/transformstream').TransformStream);
lazy('TransformStreamDefaultController', () => require('internal/webstreams/transformstream').TransformStreamDefaultController);
lazy('ByteLengthQueuingStrategy', () => require('internal/webstreams/queuingstrategies').ByteLengthQueuingStrategy);
lazy('CountQueuingStrategy', () => require('internal/webstreams/queuingstrategies').CountQueuingStrategy);
lazy('TextEncoderStream', () => require('internal/webstreams/encoding').TextEncoderStream);
lazy('TextDecoderStream', () => require('internal/webstreams/encoding').TextDecoderStream);
// a module loaded by a computed name: in the bundle only when the program's code names its
// global (as `fetch`, not `globalThis.fetch`), else undefined, and so is the global (a package
// feature-testing `globalThis.Bun` finds no Bun, as under Node.js)
function bundled(id) {
  try {
    return require(id);
  } catch (e) {
    if (e?.code === 'MODULE_NOT_FOUND' && `${e.message}`.includes(`'${id}'`)) return undefined;
    throw e;
  }
}
const pick = (mod, key) => (mod === undefined ? ABSENT : mod[key]);

// (zlib: in the bundle only when the program's code names these, so the codecs aren't linked into
// every program)
function compression() {
  const id = 'internal/webstreams/compression';
  return bundled(id);
}
lazy('CompressionStream', () => pick(compression(), 'CompressionStream'));
lazy('DecompressionStream', () => pick(compression(), 'DecompressionStream'));
lazy('Blob', () => require('internal/blob').Blob);
// fetch() and its classes (internal/barm/fetch, over Barm's HTTP client): in the bundle only
// when the program's code names them, so the TLS client isn't linked into every program
function fetchModule() {
  const id = 'internal/barm/fetch';
  return bundled(id);
}
lazy('fetch', () => pick(fetchModule(), 'fetch'));
lazy('Headers', () => pick(fetchModule(), 'Headers'));
lazy('Request', () => pick(fetchModule(), 'Request'));
lazy('Response', () => pick(fetchModule(), 'Response'));
lazy('FormData', () => pick(fetchModule(), 'FormData'));
// Bun's global (the bun module): in the bundle only when the program's code names it
lazy('Bun', () => {
  const id = 'bun';
  return bundled(id) ?? ABSENT;
});
lazy('performance', () => require('perf_hooks').performance);
lazy('PerformanceEntry', () => require('perf_hooks').PerformanceEntry);
lazy('PerformanceMark', () => require('perf_hooks').PerformanceMark);
lazy('PerformanceMeasure', () => require('perf_hooks').PerformanceMeasure);
lazy('PerformanceObserver', () => require('perf_hooks').PerformanceObserver);
lazy('PerformanceObserverEntryList', () => require('perf_hooks').PerformanceObserverEntryList);
lazy('PerformanceResourceTiming', () => require('perf_hooks').PerformanceResourceTiming);
lazy('File', () => require('internal/file').File);

// timers: Barm's event loop drives them (the natives); internal/bindings/timers wires them up
// on the first one
function timers() {
  return require('timers');
}
lazy('setTimeout', () => timers().setTimeout);
lazy('clearTimeout', () => timers().clearTimeout);
lazy('setInterval', () => timers().setInterval);
lazy('clearInterval', () => timers().clearInterval);
lazy('setImmediate', () => timers().setImmediate);
lazy('clearImmediate', () => timers().clearImmediate);
