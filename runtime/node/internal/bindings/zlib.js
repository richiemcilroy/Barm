'use strict';

// internalBinding('zlib'): Node.js's src/node_zlib.cc over Barm's natives
// (globalThis.__barm_native.zlib, runtime/node.c over runtime/compress.c). A handle's write()
// runs the codec on a later loop turn and then calls the callback init() gave it, as Node.js's
// thread pool does; writeSync() runs it now. Failures go to handle.onerror(message, errno, code).

const native = globalThis.__barm_native?.zlib;

function zlib() {
  if (native?.available) return native;
  const e = new Error('zlib is not available outside Barm\'s runtime');
  e.code = 'ERR_METHOD_NOT_IMPLEMENTED';
  throw e;
}

let setImmediate;
function later(fn) {
  setImmediate ??= require('timers').setImmediate;
  setImmediate(fn);
}

function initFailed(message) {
  const e = new Error(message);
  e.code = 'ERR_ZLIB_INITIALIZATION_FAILED';
  return e;
}

const kNative = Symbol('kNative');

class CompressionStream {
  constructor(mode) {
    this[kNative] = zlib().open(mode);
    this.writeState = null;
    this.callback = null;
    this.writeInProgress = false;
    this.pendingClose = false;
    this.closed = false;
  }

  #emit(error) {
    if (error === null || error === undefined) return true;
    this.onerror?.(error[0], error[2], error[1]);
    return false;
  }

  writeSync(flush, input, inOff, inLen, out, outOff, outLen) {
    this.#emit(native.write(this[kNative], flush, input, inOff, inLen, out, outOff, outLen, this.writeState));
  }

  write(flush, input, inOff, inLen, out, outOff, outLen) {
    this.writeInProgress = true;
    later(() => {
      this.writeInProgress = false;
      if (this.closed) return;
      const ok = this.#emit(native.write(this[kNative], flush, input, inOff, inLen, out, outOff, outLen, this.writeState));
      if (ok) this.callback.call(this);
      if (this.pendingClose) this.close();
    });
  }

  close() {
    if (this.writeInProgress) {
      this.pendingClose = true;
      return;
    }
    this.pendingClose = false;
    if (this.closed) return;
    this.closed = true;
    native.close(this[kNative]);
  }

  reset() {
    this.#emit(native.reset(this[kNative]));
  }

  params() {}

  getAsyncId() {
    return -1;
  }

  _setAsyncId() {}
}

class Zlib extends CompressionStream {
  // init(windowBits, level, memLevel, strategy, writeState, callback, dictionary)
  init(windowBits, level, memLevel, strategy, writeState, callback, dictionary) {
    this.writeState = writeState;
    this.callback = callback;
    native.initZlib(this[kNative], windowBits, level, memLevel, strategy, dictionary);
  }

  params(level, strategy) {
    const error = native.params(this[kNative], level, strategy);
    if (error) this.onerror?.(error[0], error[2], error[1]);
  }
}

class Brotli extends CompressionStream {
  // init(params, writeState, callback[, dictionary])
  init(params, writeState, callback, dictionary) {
    this.writeState = writeState;
    this.callback = callback;
    if (dictionary !== undefined && !ArrayBuffer.isView(dictionary)) {
      const { codes: { ERR_INVALID_ARG_TYPE } } = require('internal/errors');
      throw new ERR_INVALID_ARG_TYPE('dictionary', 'ArrayBufferView', dictionary);
    }
    const error = native.initBrotli(this[kNative], params, dictionary);
    if (error) {
      this.onerror?.(error[0], error[2], error[1]);
      throw initFailed('Initialization failed');
    }
  }
}

class Zstd extends CompressionStream {
  // init(params, pledgedSrcSize, writeState, callback[, dictionary])
  init(params, pledgedSrcSize, writeState, callback, dictionary) {
    this.writeState = writeState;
    this.callback = callback;
    let pledged = -1;
    if (typeof pledgedSrcSize === 'number') {
      const { codes: { ERR_INVALID_ARG_VALUE } } = require('internal/errors');
      if (!Number.isInteger(pledgedSrcSize)) throw new ERR_INVALID_ARG_VALUE('pledgedSrcSize', pledgedSrcSize, 'should be an integer');
      if (pledgedSrcSize < 0) throw new ERR_INVALID_ARG_VALUE('pledgedSrcSize', pledgedSrcSize, 'may not be negative');
      pledged = pledgedSrcSize;
    }
    const error = native.initZstd(this[kNative], params, pledged, dictionary);
    if (error) {
      this.onerror?.(error[0], error[2], error[1]);
      throw initFailed(error[0]);
    }
  }
}

module.exports = {
  Zlib,
  BrotliEncoder: class BrotliEncoder extends Brotli {},
  BrotliDecoder: class BrotliDecoder extends Brotli {},
  ZstdCompress: class ZstdCompress extends Zstd {
    constructor() {
      super(10);
    }
  },
  ZstdDecompress: class ZstdDecompress extends Zstd {
    constructor() {
      super(11);
    }
  },
  crc32(data, value) {
    if (typeof data === 'string' && !native?.available) throw new Error('zlib is not available outside Barm\'s runtime');
    return zlib().crc32(data, value >>> 0);
  },
  ZLIB_VERSION: '1.3.1',
};
