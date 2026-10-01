'use strict';

// Node.js's globals, installed before an npm bundle's first module runs (the bundler loads this
// as shim "__globals"). Each one is a getter that loads its module on first use, so a program
// pays only for the globals it touches; assigning to one replaces it, as in Node.js. (Every
// require here is a literal, so the bundler can include what it names.)

const g = globalThis;

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

// process: a plain object (it sets the global itself), with the rest of Node.js's process
// loading as it's used
require('internal/bootstrap/process');

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
lazy('CompressionStream', () => require('internal/webstreams/compression').CompressionStream);
lazy('DecompressionStream', () => require('internal/webstreams/compression').DecompressionStream);
lazy('Blob', () => require('internal/blob').Blob);
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
