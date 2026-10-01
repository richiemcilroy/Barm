'use strict';

// internalBinding('messaging'): DOMException, structuredClone, and in-process message ports
// (MessageChannel; worker_threads will carry them across threads). A message is cloned when
// posted and delivered in a later task, as Node.js delivers it.

const { DOMException, QuotaExceededError } = require('internal/per_context/domexception');
const { emitMessage } = require('internal/per_context/messageport');
const symbols = require('internal/bindings/symbols');

const { oninit: onInitSymbol, handle_onclose: handleOnCloseSymbol } = symbols;

function dataCloneError(what) {
  return new DOMException(`${what} could not be cloned.`, 'DataCloneError');
}

const getTag = (v) => Object.prototype.toString.call(v).slice(8, -1);
const typedArrays = {
  Int8Array, Uint8Array, Uint8ClampedArray, Int16Array, Uint16Array, Int32Array, Uint32Array,
  Float32Array, Float64Array, BigInt64Array, BigUint64Array,
};

// The HTML structured clone algorithm (no transfer lists yet)
function structuredClone(value, options) {
  const seen = new Map();
  const clone = (v) => {
    if (v === null || (typeof v !== 'object' && typeof v !== 'function')) {
      if (typeof v === 'symbol') throw dataCloneError(String(v));
      return v;
    }
    if (typeof v === 'function') throw dataCloneError(`${v}`.split('\n')[0]);
    if (seen.has(v)) return seen.get(v);
    const tag = getTag(v);
    let out;
    switch (tag) {
      case 'Date': out = new Date(v.getTime()); break;
      case 'RegExp': out = new RegExp(v.source, v.flags); break;
      case 'Boolean': out = Object(Boolean.prototype.valueOf.call(v)); break;
      case 'Number': out = Object(Number.prototype.valueOf.call(v)); break;
      case 'String': out = Object(String.prototype.valueOf.call(v)); break;
      case 'BigInt': out = Object(BigInt.prototype.valueOf.call(v)); break;
      case 'ArrayBuffer': out = v.slice(0); break;
      case 'SharedArrayBuffer': out = v; break;
      case 'DataView': {
        out = new DataView(clone(v.buffer), v.byteOffset, v.byteLength);
        break;
      }
      case 'Map': {
        out = new Map();
        seen.set(v, out);
        for (const [k, x] of v) out.set(clone(k), clone(x));
        return out;
      }
      case 'Set': {
        out = new Set();
        seen.set(v, out);
        for (const x of v) out.add(clone(x));
        return out;
      }
      case 'Error': {
        const Ctor = { EvalError, RangeError, ReferenceError, SyntaxError, TypeError, URIError }[v.name] ?? Error;
        out = new Ctor(v.message);
        seen.set(v, out);
        if ('stack' in v) Object.defineProperty(out, 'stack', { value: v.stack, writable: true, configurable: true });
        if ('cause' in v) out.cause = clone(v.cause);
        return out;
      }
      case 'Array': {
        out = new Array(v.length);
        seen.set(v, out);
        for (const k of Object.keys(v)) out[k] = clone(v[k]);
        return out;
      }
      case 'Object': {
        if (v instanceof Promise || v instanceof WeakMap || v instanceof WeakSet) throw dataCloneError(`#<${tag}>`);
        out = {};
        seen.set(v, out);
        for (const k of Object.keys(v)) out[k] = clone(v[k]);
        return out;
      }
      default: {
        const T = typedArrays[tag];
        if (T) {
          out = new T(clone(v.buffer), v.byteOffset, v.length);
          break;
        }
        throw dataCloneError(`#<${tag}>`);
      }
    }
    seen.set(v, out);
    return out;
  };
  return clone(value);
}

const later = (fn) => (typeof setImmediate === 'function' ? setImmediate(fn) : setTimeout(fn, 0));
const kPeer = Symbol('kPeer');
const kClosed = Symbol('kClosed');
const kRef = Symbol('kRef');
let constructing = false;

class MessagePort {
  constructor() {
    if (!constructing) throw new TypeError('Illegal constructor');
    this[kPeer] = null;
    this[kClosed] = false;
    this[kRef] = true;
    if (typeof this[onInitSymbol] === 'function') this[onInitSymbol]();
  }

  postMessage(value) {
    if (this[kClosed]) return;
    const data = structuredClone(value);
    const peer = this[kPeer];
    if (peer === null) return;
    later(() => {
      if (!peer[kClosed]) emitMessage.call(peer, data, [], 'message');
    });
  }

  start() {}

  close() {
    if (this[kClosed]) return;
    for (const port of [this, this[kPeer]]) {
      if (port === null || port[kClosed]) continue;
      port[kClosed] = true;
      later(() => port[handleOnCloseSymbol]?.());
    }
  }

  ref() { this[kRef] = true; }
  unref() { this[kRef] = false; }
  hasRef() { return this[kRef] && !this[kClosed]; }
}

function makePort() {
  constructing = true;
  try {
    return new MessagePort();
  } finally {
    constructing = false;
  }
}

class MessageChannel {
  constructor() {
    this.port1 = makePort();
    this.port2 = makePort();
    this.port1[kPeer] = this.port2;
    this.port2[kPeer] = this.port1;
  }
}

module.exports = {
  DOMException,
  QuotaExceededError,
  structuredClone,
  MessagePort,
  MessageChannel,
  broadcastChannel: () => makePort(),
  drainMessagePort() {},
  moveMessagePortToContext: (port) => port,
  receiveMessageOnPort: () => undefined,
  stopMessagePort() {},
  setDeserializerCreateObjectFunction() {},
  checkMessagePort: (v) => v instanceof MessagePort,
};
