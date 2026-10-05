'use strict';

// internalBinding('stream_wrap'): libuv streams as Node.js's src/stream_wrap.cc and
// stream_base.cc expose them, over Tov's native stream handles
// (globalThis.__tov_native.stream, in runtime/node.c). LibuvStreamWrap is the base of Pipe
// (pipe_wrap) and, later, TCP: reads arrive in onread(arrayBuffer) with streamBaseState saying
// how many bytes (or the error); writes report their bytes and whether they finished now
// through streamBaseState, and call req.oncomplete when they finish later.

const native = globalThis.__tov_native?.stream;

const kReadBytesOrError = 0;
const kArrayBufferOffset = 1;
const kBytesWritten = 2;
const kLastWriteWasAsync = 3;
const streamBaseState = new Int32Array(4);

const kNative = Symbol('kNative');

// a callback from the loop: then process.nextTick's queue, as Node.js's MakeCallback does
const { callFromHost } = require('internal/bindings/task_queue');
const complete = (fn) => callFromHost(fn);

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
  _tovOpen(fd) {
    this[kNative] = native.open(fd);
    if (this._tovUnref) native.ref(this[kNative], false);
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
    this._tovUnref = false;
    if (this[kNative]) native.ref(this[kNative], true);
  }

  unref() {
    this._tovUnref = true;
    if (this[kNative]) native.ref(this[kNative], false);
  }

  hasRef() {
    return !this._tovUnref;
  }

  getAsyncId() {
    return -1;
  }

  getProviderType() {
    return 0;
  }

  asyncReset() {}
}

// a socket (TCP, or a unix socket for Pipe): made when it's first bound or connected, in the
// address's family (4, 6, or 0 for a unix path)
class SocketWrap extends LibuvStreamWrap {
  // (the family a new socket is made in: subclasses)
  _tovSocket(family) {
    if (this[kNative]) return 0;
    const fd = native.socket(family);
    if (fd < 0) return fd;
    return this._tovOpen(fd);
  }

  _tovBind(address, port, family, ipv6Only) {
    const err = this._tovSocket(family);
    return err || native.bind(this.fd, address, port, family, ipv6Only);
  }

  listen(backlog) {
    if (!this[kNative]) return -22; // UV_EINVAL
    return native.listen(this[kNative], backlog, (fd, err) => {
      complete(() => {
        if (fd === null) {
          this.onconnection(err);
          return;
        }
        const client = new this.constructor(0);
        client._tovOpen(fd);
        this.onconnection(0, client);
      });
    });
  }

  _tovConnect(req, address, port, family) {
    const err = this._tovSocket(family);
    if (err) return err;
    return native.connect(this[kNative], address, port, family, (status) => {
      complete(() => req.oncomplete(status, this, req, true, true));
    });
  }

  #name(out, peer) {
    if (!this[kNative]) return -9; // UV_EBADF
    const r = native.name(this.fd, peer);
    if (typeof r === 'number') return r;
    out.address = r[0];
    out.family = r[1];
    out.port = r[2];
    return 0;
  }

  getsockname(out) {
    return this.#name(out, false);
  }

  getpeername(out) {
    return this.#name(out, true);
  }
}

module.exports = {
  SocketWrap,
  kNative,
  LibuvStreamWrap,
  WriteWrap,
  ShutdownWrap,
  kReadBytesOrError,
  kArrayBufferOffset,
  kBytesWritten,
  kLastWriteWasAsync,
  streamBaseState,
};
