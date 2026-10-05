'use strict';

// Web Crypto's getRandomValues and randomUUID (the global `crypto`, and crypto's): secure random
// bytes from the natives (BoringSSL when the program links it, else the system's arc4random),
// without loading the rest of crypto.

const native = globalThis.__tov_native?.crypto;

function randomFillBytes(bytes) {
  if (native) {
    native.randomFill(bytes);
    return;
  }
  const e = new Error('Secure random numbers are not available outside Tov\'s runtime');
  e.code = 'ERR_METHOD_NOT_IMPLEMENTED';
  throw e;
}

const integerArrays = ['Int8Array', 'Int16Array', 'Int32Array', 'Uint8Array', 'Uint8ClampedArray', 'Uint16Array', 'Uint32Array', 'BigInt64Array', 'BigUint64Array'];

function getRandomValues(data) {
  const { DOMException, QuotaExceededError } = require('internal/bindings/messaging');
  const tag = ArrayBuffer.isView(data) && !(data instanceof DataView) ? Object.prototype.toString.call(data).slice(8, -1) : '';
  if (!integerArrays.includes(tag)) {
    throw new DOMException('The data argument must be an integer-type TypedArray', 'TypeMismatchError');
  }
  if (data.byteLength > 65536) {
    throw new QuotaExceededError('The requested length exceeds 65,536 bytes', { quota: 65536, requested: data.byteLength });
  }
  randomFillBytes(new Uint8Array(data.buffer, data.byteOffset, data.byteLength));
  return data;
}

const hex = [];
for (let i = 0; i < 256; i++) hex.push((i + 0x100).toString(16).slice(1));

// a version 4 UUID
function randomUUID() {
  const b = new Uint8Array(16);
  randomFillBytes(b);
  b[6] = (b[6] & 0x0f) | 0x40;
  b[8] = (b[8] & 0x3f) | 0x80;
  return `${hex[b[0]]}${hex[b[1]]}${hex[b[2]]}${hex[b[3]]}-${hex[b[4]]}${hex[b[5]]}-${hex[b[6]]}${hex[b[7]]}-${hex[b[8]]}${hex[b[9]]}-${hex[b[10]]}${hex[b[11]]}${hex[b[12]]}${hex[b[13]]}${hex[b[14]]}${hex[b[15]]}`;
}

module.exports = { getRandomValues, randomUUID, randomFillBytes };
