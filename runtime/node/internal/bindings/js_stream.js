'use strict';

// internalBinding('js_stream'): JSStream (Node.js's src/js_stream.cc), a stream handle whose
// other end is JavaScript (JSStreamSocket wraps a duplex in one, for tls over any stream). Its
// reads come in through readBuffer() and emitEOF(); writes and shutdowns go out through
// onwrite and onshutdown, finished with finishWrite() and finishShutdown().

const { streamBaseState, kReadBytesOrError, kArrayBufferOffset, kBytesWritten, kLastWriteWasAsync } = require('internal/bindings/stream_wrap');

class JSStream {
  constructor() {
    this.isStreamBase = true;
    this.reading = false;
    this.bytesRead = 0;
    this.bytesWritten = 0;
    this.fd = -1;
    this._externalStream = undefined;
  }

  readStart() {
    this.reading = true;
    return this.onreadstart?.() ?? 0;
  }

  readStop() {
    this.reading = false;
    return this.onreadstop?.() ?? 0;
  }

  readBuffer(chunk) {
    this.bytesRead += chunk.byteLength;
    streamBaseState[kReadBytesOrError] = chunk.byteLength;
    streamBaseState[kArrayBufferOffset] = chunk.byteOffset;
    this.onread?.(chunk.buffer);
  }

  emitEOF() {
    streamBaseState[kReadBytesOrError] = -4095; // UV_EOF
    this.onread?.(undefined);
  }

  #write(req, chunks) {
    const bytes = chunks.reduce((n, c) => n + (typeof c === 'string' ? Buffer.byteLength(c) : c.byteLength), 0);
    this.bytesWritten += bytes;
    streamBaseState[kBytesWritten] = bytes;
    streamBaseState[kLastWriteWasAsync] = 1;
    return this.onwrite?.(req, chunks) ?? 0;
  }

  writeBuffer(req, data) {
    return this.#write(req, [data]);
  }

  writev(req, chunks, allBuffers) {
    const list = [];
    if (allBuffers) list.push(...chunks);
    else for (let i = 0; i < chunks.length; i += 2) list.push(Buffer.from(chunks[i], chunks[i + 1]));
    return this.#write(req, list);
  }

  writeUtf8String(req, s) {
    return this.#write(req, [Buffer.from(s, 'utf8')]);
  }

  writeAsciiString(req, s) {
    return this.#write(req, [Buffer.from(s, 'latin1')]);
  }

  writeLatin1String(req, s) {
    return this.#write(req, [Buffer.from(s, 'latin1')]);
  }

  writeUcs2String(req, s) {
    return this.#write(req, [Buffer.from(s, 'ucs2')]);
  }

  finishWrite(req, errCode) {
    req.oncomplete(errCode, this, undefined);
  }

  shutdown(req) {
    return this.onshutdown?.(req) ?? 0;
  }

  finishShutdown(req, errCode) {
    req.oncomplete(errCode);
  }

  useUserBuffer() {}

  close(callback) {
    if (typeof callback === 'function') queueMicrotask(() => callback.call(this));
  }

  ref() {}

  unref() {}

  hasRef() {
    return true;
  }

  getAsyncId() {
    return -1;
  }
}

const { Buffer } = require('buffer');

module.exports = { JSStream };
