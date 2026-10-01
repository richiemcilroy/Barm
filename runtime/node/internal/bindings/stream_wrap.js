'use strict';

// internalBinding('stream_wrap'): libuv streams as Node.js's src/stream_wrap.cc and
// stream_base.cc expose them, over Barm's native stream handles
// (globalThis.__barm_native.stream, in runtime/node.c). LibuvStreamWrap is the base of Pipe
// (pipe_wrap) and, later, TCP: reads arrive in onread(arrayBuffer) with streamBaseState saying
// how many bytes (or the error); writes report their bytes and whether they finished now
// through streamBaseState, and call req.oncomplete when they finish later.

const native = globalThis.__barm_native?.stream;

const kReadBytesOrError = 0;
const kArrayBufferOffset = 1;
const kBytesWritten = 2;
const kLastWriteWasAsync = 3;
const streamBaseState = new Int32Array(4);

const kNative = Symbol('kNative');

let runTicks;
// a callback from the loop: then process.nextTick's queue, as Node.js's MakeCallback does
function complete(fn) {
  runTicks ??= require('internal/bindings/task_queue').runTicks;
  try {
    fn();
  } finally {
    runTicks();
  }
}

let setImmediate;
function later(fn) {
  setImmediate ??= require('timers').setImmediate;
  setImmediate(fn);
}

let Buffer;
function bytes(chunk, encoding) {
  if (typeof chunk !== 'string') return chunk;
  Buffer ??= require('buffer').Buffer;
  return Buffer.from(chunk, encoding);
}

class WriteWrap {}
class ShutdownWrap {}

// HandleWrap and LibuvStreamWrap, over a native handle set by [kNative]
class LibuvStreamWrap {
  constructor() {
    this[kNative] = null;
    this.reading = false;
    this.onread = undefined;
    this.isStreamBase = true;
  }

  // (for subclasses: the stream is this fd from now on)
  _barmOpen(fd) {
    this[kNative] = native.open(fd);
    if (this._barmUnref) native.ref(this[kNative], false);
    return 0;
  }

  get fd() {
    return this[kNative] ? native.info(this[kNative])[0] : -1;
  }

  get bytesRead() {
    return this[kNative] ? native.info(this[kNative])[1] : 0;
  }

  get bytesWritten() {
    return this[kNative] ? native.info(this[kNative])[2] : 0;
  }

  get writeQueueSize() {
    return this[kNative] ? native.info(this[kNative])[3] : 0;
  }

  get _externalStream() {
    return undefined;
  }

  readStart() {
    if (!this[kNative]) return -9; // UV_EBADF
    return native.readStart(this[kNative], (chunk, err) => {
      complete(() => {
        if (chunk) {
          streamBaseState[kReadBytesOrError] = chunk.byteLength;
          streamBaseState[kArrayBufferOffset] = chunk.byteOffset;
          this.onread(chunk.buffer);
        } else {
          streamBaseState[kReadBytesOrError] = err;
          streamBaseState[kArrayBufferOffset] = 0;
          this.onread(undefined);
        }
      });
    });
  }

  readStop() {
    return this[kNative] ? native.readStop(this[kNative]) : 0;
  }

  #write(req, list) {
    if (!this[kNative]) {
      streamBaseState[kBytesWritten] = 0;
      streamBaseState[kLastWriteWasAsync] = 0;
      return -9;
    }
    const [err, written, async] = native.write(this[kNative], list, (status) => {
      complete(() => req.oncomplete(status, this, undefined));
    });
    streamBaseState[kBytesWritten] = written;
    streamBaseState[kLastWriteWasAsync] = async ? 1 : 0;
    return err;
  }

  writeBuffer(req, data) {
    return this.#write(req, [data]);
  }

  // chunks: buffers (allBuffers), else [chunk, encoding, chunk, encoding, ...]
  writev(req, chunks, allBuffers) {
    const list = [];
    if (allBuffers) {
      for (const c of chunks) list.push(c);
    } else {
      for (let i = 0; i < chunks.length; i += 2) list.push(bytes(chunks[i], chunks[i + 1]));
    }
    return this.#write(req, list);
  }

  writeUtf8String(req, s) {
    return this.#write(req, [bytes(s, 'utf8')]);
  }

  writeAsciiString(req, s) {
    return this.#write(req, [bytes(s, 'latin1')]);
  }

  writeLatin1String(req, s) {
    return this.#write(req, [bytes(s, 'latin1')]);
  }

  writeUcs2String(req, s) {
    return this.#write(req, [bytes(s, 'ucs2')]);
  }

  shutdown(req) {
    if (!this[kNative]) return -57; // UV_ENOTCONN
    return native.shutdown(this[kNative], (status) => {
      complete(() => req.oncomplete(status));
    });
  }

  useUserBuffer() {}

  setBlocking() {
    return 0;
  }

  // HandleWrap
  close(callback) {
    const h = this[kNative];
    if (h) native.close(h);
    later(() => {
      if (h) native.release(h);
      if (typeof callback === 'function') complete(() => callback.call(this));
    });
  }

  ref() {
    this._barmUnref = false;
    if (this[kNative]) native.ref(this[kNative], true);
  }

  unref() {
    this._barmUnref = true;
    if (this[kNative]) native.ref(this[kNative], false);
  }

  hasRef() {
    return !this._barmUnref;
  }

  getAsyncId() {
    return -1;
  }

  getProviderType() {
    return 0;
  }

  asyncReset() {}
}

module.exports = {
  LibuvStreamWrap,
  WriteWrap,
  ShutdownWrap,
  kReadBytesOrError,
  kArrayBufferOffset,
  kBytesWritten,
  kLastWriteWasAsync,
  streamBaseState,
};
