'use strict';

// crypto (Barm's own; Node.js's binds all of OpenSSL): hashes, HMAC, random values, key
// derivation and timingSafeEqual, on BoringSSL through the natives
// (globalThis.__barm_native.crypto, runtime/node.c over runtime/crypto.c). It answers and fails
// as Node.js does. Ciphers, signatures and key objects aren't there yet: those functions throw
// ERR_METHOD_NOT_IMPLEMENTED.

const { Buffer } = require('buffer');
const {
  codes: {
    ERR_CRYPTO_HASH_FINALIZED,
    ERR_CRYPTO_INVALID_DIGEST,
    ERR_INVALID_ARG_TYPE,
    ERR_METHOD_NOT_IMPLEMENTED,
    ERR_OUT_OF_RANGE,
  },
} = require('internal/errors');
const {
  validateFunction,
  validateInt32,
  validateNumber,
  validateObject,
  validateString,
} = require('internal/validators');
const { isArrayBufferView, isAnyArrayBuffer } = require('internal/util/types');
const LazyTransform = require('internal/streams/lazy_transform');
const { getRandomValues, randomUUID, randomFillBytes } = require('internal/barm/random');

// (errors Node.js raises from C++, so internal/errors doesn't have them)
function nativeError(Base, code, message) {
  const e = new Base(message);
  e.code = code;
  return e;
}

const native = globalThis.__barm_native?.crypto;
function crypto() {
  if (native?.available) return native;
  const e = new Error('crypto is not available outside Barm\'s runtime');
  e.code = 'ERR_METHOD_NOT_IMPLEMENTED';
  throw e;
}

let setImmediate;
function later(fn) {
  setImmediate ??= require('timers').setImmediate;
  setImmediate(fn);
}

// data as bytes: a string in its encoding, or a view's bytes
function toBytes(data, encoding, name = 'data') {
  if (typeof data === 'string') return Buffer.from(data, encoding ?? 'utf8');
  if (isArrayBufferView(data)) return new Uint8Array(data.buffer, data.byteOffset, data.byteLength);
  if (isAnyArrayBuffer(data)) return new Uint8Array(data);
  throw new ERR_INVALID_ARG_TYPE(name, ['string', 'Buffer', 'TypedArray', 'DataView'], data);
}

function output(bytes, encoding) {
  const buf = Buffer.from(bytes.buffer, bytes.byteOffset, bytes.byteLength);
  return encoding === undefined || encoding === 'buffer' ? buf : buf.toString(encoding);
}

const kHandle = Symbol('kHandle');
const kFinalized = Symbol('kFinalized');

function Hash(algorithm, options, handle) {
  if (!(this instanceof Hash)) return new Hash(algorithm, options);
  if (handle === undefined) {
    validateString(algorithm, 'algorithm');
    handle = crypto().hashNew(algorithm);
    if (handle === null) throw new Error('Digest method not supported');
  }
  this[kHandle] = handle;
  this[kFinalized] = false;
  LazyTransform.call(this, options);
}
Object.setPrototypeOf(Hash.prototype, LazyTransform.prototype);
Object.setPrototypeOf(Hash, LazyTransform);

Hash.prototype.copy = function copy(options) {
  if (this[kFinalized]) throw new ERR_CRYPTO_HASH_FINALIZED();
  return new Hash(undefined, options, crypto().hashCopy(this[kHandle]));
};

Hash.prototype._transform = function _transform(chunk, encoding, callback) {
  crypto().update(this[kHandle], toBytes(chunk, encoding));
  callback();
};

Hash.prototype._flush = function _flush(callback) {
  this.push(this.digest());
  callback();
};

Hash.prototype.update = function update(data, encoding) {
  if (this[kFinalized]) throw new ERR_CRYPTO_HASH_FINALIZED();
  crypto().update(this[kHandle], toBytes(data, encoding));
  return this;
};

Hash.prototype.digest = function digest(encoding) {
  if (this[kFinalized]) throw new ERR_CRYPTO_HASH_FINALIZED();
  this[kFinalized] = true;
  return output(crypto().digest(this[kHandle]), encoding);
};

function Hmac(hmac, key, options) {
  if (!(this instanceof Hmac)) return new Hmac(hmac, key, options);
  validateString(hmac, 'hmac');
  const keyBytes = toBytes(key, options?.encoding, 'key');
  const handle = crypto().hmacNew(hmac, keyBytes);
  if (handle === null) throw new ERR_CRYPTO_INVALID_DIGEST(hmac);
  this[kHandle] = handle;
  this[kFinalized] = false;
  LazyTransform.call(this, options);
}
Object.setPrototypeOf(Hmac.prototype, LazyTransform.prototype);
Object.setPrototypeOf(Hmac, LazyTransform);

Hmac.prototype.update = Hash.prototype.update;
Hmac.prototype._transform = Hash.prototype._transform;
Hmac.prototype._flush = Hash.prototype._flush;
// (a second digest() gives an empty result, as in Node.js)
Hmac.prototype.digest = function digest(encoding) {
  if (this[kFinalized]) return output(new Uint8Array(0), encoding);
  this[kFinalized] = true;
  return output(crypto().digest(this[kHandle]), encoding);
};

function createHash(algorithm, options) {
  return new Hash(algorithm, options);
}

function createHmac(hmac, key, options) {
  return new Hmac(hmac, key, options);
}

// crypto.hash(algorithm, data, outputEncoding = 'hex'): one-shot
function hash(algorithm, data, outputEncoding = 'hex') {
  validateString(algorithm, 'algorithm');
  if (typeof data !== 'string' && !isArrayBufferView(data)) throw new ERR_INVALID_ARG_TYPE('data', ['Buffer', 'TypedArray', 'DataView', 'string'], data);
  const h = crypto().hashNew(algorithm);
  if (h === null) throw new Error('Digest method not supported');
  crypto().update(h, toBytes(data));
  return output(crypto().digest(h), outputEncoding);
}

const kMaxRandomSize = 2 ** 31 - 1;

function assertSize(size, name = 'size', max = kMaxRandomSize) {
  validateNumber(size, name);
  if (!Number.isInteger(size) || size < 0 || size > max) throw new ERR_OUT_OF_RANGE(name, `>= 0 && <= ${max}`, size);
}

function randomBytes(size, callback) {
  assertSize(size);
  if (callback !== undefined) validateFunction(callback, 'callback');
  const buf = Buffer.allocUnsafe(size);
  randomFillBytes(buf);
  if (callback === undefined) return buf;
  later(() => callback(null, buf));
}

function viewOf(buf) {
  if (isAnyArrayBuffer(buf)) return new Uint8Array(buf);
  if (!isArrayBufferView(buf)) throw new ERR_INVALID_ARG_TYPE('buf', ['ArrayBuffer', 'ArrayBufferView'], buf);
  return new Uint8Array(buf.buffer, buf.byteOffset, buf.byteLength);
}

function randomFillSync(buf, offset = 0, size) {
  const bytes = viewOf(buf);
  const elementSize = isArrayBufferView(buf) ? (buf.BYTES_PER_ELEMENT ?? 1) : 1;
  assertSize(offset * elementSize, 'offset', bytes.length);
  const start = offset * elementSize;
  const n = size === undefined ? bytes.length - start : size * elementSize;
  assertSize(n, 'size', bytes.length - start);
  randomFillBytes(bytes.subarray(start, start + n));
  return buf;
}

function randomFill(buf, offset, size, callback) {
  if (typeof offset === 'function') {
    callback = offset;
    offset = 0;
    size = undefined;
  } else if (typeof size === 'function') {
    callback = size;
    size = undefined;
  }
  validateFunction(callback, 'callback');
  randomFillSync(buf, offset, size);
  later(() => callback(null, buf));
}

// Node.js's randomInt: rejection sampling over 48-bit random values
const RAND_MAX = 0xFFFF_FFFF_FFFF;
function randomInt(min, max, callback) {
  const minNotSpecified = typeof max === 'undefined' || typeof max === 'function';
  if (minNotSpecified) {
    callback = max;
    max = min;
    min = 0;
  }
  if (callback !== undefined) validateFunction(callback, 'callback');
  if (!Number.isSafeInteger(min)) throw new ERR_INVALID_ARG_TYPE('min', 'a safe integer', min);
  if (!Number.isSafeInteger(max)) throw new ERR_INVALID_ARG_TYPE('max', 'a safe integer', max);
  if (max <= min) throw new ERR_OUT_OF_RANGE('max', `greater than the value of "min" (${min})`, max);
  const range = max - min;
  if (!(range <= RAND_MAX)) throw new ERR_OUT_OF_RANGE(`max${minNotSpecified ? '' : ' - min'}`, `<= ${RAND_MAX}`, range);
  const randLimit = RAND_MAX - (RAND_MAX % range);
  const six = new Uint8Array(6);
  let x;
  do {
    randomFillBytes(six);
    x = six[0] * 2 ** 40 + six[1] * 2 ** 32 + ((six[2] << 24) >>> 0) + (six[3] << 16) + (six[4] << 8) + six[5];
  } while (x >= randLimit);
  const n = (x % range) + min;
  if (callback === undefined) return n;
  later(() => callback(null, n));
}

function timingSafeEqual(a, b) {
  if (!isAnyArrayBuffer(a) && !isArrayBufferView(a)) throw new ERR_INVALID_ARG_TYPE('buf1', ['ArrayBuffer', 'Buffer', 'TypedArray', 'DataView'], a);
  if (!isAnyArrayBuffer(b) && !isArrayBufferView(b)) throw new ERR_INVALID_ARG_TYPE('buf2', ['ArrayBuffer', 'Buffer', 'TypedArray', 'DataView'], b);
  const x = viewOf(a);
  const y = viewOf(b);
  if (x.byteLength !== y.byteLength) throw nativeError(RangeError, 'ERR_CRYPTO_TIMING_SAFE_EQUAL_LENGTH', 'Input buffers must have the same byte length');
  return native ? native.timingSafeEqual(x, y) : x.every((v, i) => v === y[i]);
}

function pbkdf2Sync(password, salt, iterations, keylen, digest) {
  const pass = toBytes(password, undefined, 'password');
  const s = toBytes(salt, undefined, 'salt');
  validateInt32(iterations, 'iterations', 1);
  validateInt32(keylen, 'keylen', 0);
  validateString(digest, 'digest');
  const out = crypto().pbkdf2(digest, pass, s, iterations, keylen);
  if (out === null) throw new ERR_CRYPTO_INVALID_DIGEST(digest);
  return Buffer.from(out.buffer, out.byteOffset, out.byteLength);
}

function pbkdf2(password, salt, iterations, keylen, digest, callback) {
  if (typeof digest === 'function') {
    callback = digest;
    digest = undefined;
  }
  validateFunction(callback, 'callback');
  const key = pbkdf2Sync(password, salt, iterations, keylen, digest);
  later(() => callback(null, key));
}

const scryptDefaults = { N: 16384, r: 8, p: 1, maxmem: 32 << 20 };
function scryptParams(options) {
  const o = options ?? {};
  if (options !== undefined) validateObject(options, 'options', { nullable: true });
  const N = o.N ?? o.cost ?? scryptDefaults.N;
  const r = o.r ?? o.blockSize ?? scryptDefaults.r;
  const p = o.p ?? o.parallelization ?? scryptDefaults.p;
  const maxmem = o.maxmem ?? scryptDefaults.maxmem;
  return { N, r, p, maxmem };
}

function scryptSync(password, salt, keylen, options) {
  const pass = toBytes(password, undefined, 'password');
  const s = toBytes(salt, undefined, 'salt');
  validateInt32(keylen, 'keylen', 0);
  const { N, r, p, maxmem } = scryptParams(options);
  const out = crypto().scrypt(pass, s, N, r, p, maxmem, keylen);
  if (out === null) throw nativeError(RangeError, 'ERR_CRYPTO_INVALID_SCRYPT_PARAMS', 'Invalid scrypt params');
  return Buffer.from(out.buffer, out.byteOffset, out.byteLength);
}

function scrypt(password, salt, keylen, options, callback) {
  if (typeof options === 'function') {
    callback = options;
    options = undefined;
  }
  validateFunction(callback, 'callback');
  let key;
  try {
    key = scryptSync(password, salt, keylen, options);
  } catch (e) {
    later(() => callback(e));
    return;
  }
  later(() => callback(null, key));
}

// hkdfSync returns an ArrayBuffer, as Node.js's does
function hkdfSync(digest, ikm, salt, info, keylen) {
  validateString(digest, 'digest');
  const key = toBytes(ikm, undefined, 'ikm');
  const s = toBytes(salt, undefined, 'salt');
  const i = toBytes(info, undefined, 'info');
  validateInt32(keylen, 'length', 0);
  const out = crypto().hkdf(digest, key, s, i, keylen);
  if (out === null) throw new ERR_CRYPTO_INVALID_DIGEST(digest);
  return out.buffer.slice(out.byteOffset, out.byteOffset + out.byteLength);
}

function hkdf(digest, ikm, salt, info, keylen, callback) {
  validateFunction(callback, 'callback');
  const out = hkdfSync(digest, ikm, salt, info, keylen);
  later(() => callback(null, out));
}

// what BoringSSL has
const hashes = ['RSA-MD5', 'RSA-SHA1', 'RSA-SHA224', 'RSA-SHA256', 'RSA-SHA384', 'RSA-SHA512', 'RSA-SHA512/256', 'md5', 'md5-sha1', 'sha1', 'sha224', 'sha256', 'sha384', 'sha512', 'sha512-256'];
const getHashes = () => hashes.slice();
const getCiphers = () => [];
const getCurves = () => [];

// Web Crypto: getRandomValues and randomUUID, and subtle.digest
const subtle = {
  async digest(algorithm, data) {
    const name = typeof algorithm === 'string' ? algorithm : algorithm?.name;
    const h = crypto().hashNew(String(name));
    if (h === null) {
      const { DOMException } = require('internal/bindings/messaging');
      throw new DOMException('Unrecognized algorithm name', 'NotSupportedError');
    }
    crypto().update(h, viewOf(data));
    const out = crypto().digest(h);
    return out.buffer.slice(out.byteOffset, out.byteOffset + out.byteLength);
  },
};
for (const m of ['encrypt', 'decrypt', 'sign', 'verify', 'generateKey', 'deriveKey', 'deriveBits', 'importKey', 'exportKey', 'wrapKey', 'unwrapKey']) {
  subtle[m] = async () => {
    const { DOMException } = require('internal/bindings/messaging');
    throw new DOMException(`crypto.subtle.${m} is not supported in Barm yet`, 'NotSupportedError');
  };
}
const webcrypto = { getRandomValues, randomUUID, subtle, CryptoKey: class CryptoKey {} };

function notYet(name) {
  return function () {
    throw new ERR_METHOD_NOT_IMPLEMENTED(`crypto.${name}`);
  };
}

module.exports = {
  createHash,
  createHmac,
  hash,
  Hash,
  Hmac,
  randomBytes,
  pseudoRandomBytes: randomBytes,
  prng: randomBytes,
  rng: randomBytes,
  randomFill,
  randomFillSync,
  randomInt,
  randomUUID,
  getRandomValues,
  timingSafeEqual,
  pbkdf2,
  pbkdf2Sync,
  scrypt,
  scryptSync,
  hkdf,
  hkdfSync,
  getHashes,
  getCiphers,
  getCurves,
  getFips: () => 0,
  setFips() {},
  constants: {"OPENSSL_VERSION_NUMBER":811597856,"SSL_OP_ALL":2147485776,"SSL_OP_ALLOW_NO_DHE_KEX":1024,"SSL_OP_ALLOW_UNSAFE_LEGACY_RENEGOTIATION":262144,"SSL_OP_CIPHER_SERVER_PREFERENCE":4194304,"SSL_OP_CISCO_ANYCONNECT":32768,"SSL_OP_COOKIE_EXCHANGE":8192,"SSL_OP_CRYPTOPRO_TLSEXT_BUG":2147483648,"SSL_OP_DONT_INSERT_EMPTY_FRAGMENTS":2048,"SSL_OP_LEGACY_SERVER_CONNECT":4,"SSL_OP_NO_COMPRESSION":131072,"SSL_OP_NO_ENCRYPT_THEN_MAC":524288,"SSL_OP_NO_QUERY_MTU":4096,"SSL_OP_NO_RENEGOTIATION":1073741824,"SSL_OP_NO_SESSION_RESUMPTION_ON_RENEGOTIATION":65536,"SSL_OP_NO_SSLv2":0,"SSL_OP_NO_SSLv3":33554432,"SSL_OP_NO_TICKET":16384,"SSL_OP_NO_TLSv1":67108864,"SSL_OP_NO_TLSv1_1":268435456,"SSL_OP_NO_TLSv1_2":134217728,"SSL_OP_NO_TLSv1_3":536870912,"SSL_OP_PRIORITIZE_CHACHA":2097152,"SSL_OP_TLS_ROLLBACK_BUG":8388608,"ENGINE_METHOD_RSA":1,"ENGINE_METHOD_DSA":2,"ENGINE_METHOD_DH":4,"ENGINE_METHOD_RAND":8,"ENGINE_METHOD_EC":2048,"ENGINE_METHOD_CIPHERS":64,"ENGINE_METHOD_DIGESTS":128,"ENGINE_METHOD_PKEY_METHS":512,"ENGINE_METHOD_PKEY_ASN1_METHS":1024,"ENGINE_METHOD_ALL":65535,"ENGINE_METHOD_NONE":0,"DH_CHECK_P_NOT_SAFE_PRIME":2,"DH_CHECK_P_NOT_PRIME":1,"DH_UNABLE_TO_CHECK_GENERATOR":4,"DH_NOT_SUITABLE_GENERATOR":8,"RSA_PKCS1_PADDING":1,"RSA_NO_PADDING":3,"RSA_PKCS1_OAEP_PADDING":4,"RSA_X931_PADDING":5,"RSA_PKCS1_PSS_PADDING":6,"RSA_PSS_SALTLEN_DIGEST":-1,"RSA_PSS_SALTLEN_MAX_SIGN":-2,"RSA_PSS_SALTLEN_AUTO":-2,"defaultCoreCipherList":"TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-SHA256:DHE-RSA-AES128-SHA256:ECDHE-RSA-AES256-SHA384:DHE-RSA-AES256-SHA384:ECDHE-RSA-AES256-SHA256:DHE-RSA-AES256-SHA256:HIGH:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!MD5:!PSK:!SRP:!CAMELLIA","TLS1_VERSION":769,"TLS1_1_VERSION":770,"TLS1_2_VERSION":771,"TLS1_3_VERSION":772,"POINT_CONVERSION_COMPRESSED":2,"POINT_CONVERSION_UNCOMPRESSED":4,"POINT_CONVERSION_HYBRID":6,"defaultCipherList":"TLS_AES_256_GCM_SHA384:TLS_CHACHA20_POLY1305_SHA256:TLS_AES_128_GCM_SHA256:ECDHE-RSA-AES128-GCM-SHA256:ECDHE-ECDSA-AES128-GCM-SHA256:ECDHE-RSA-AES256-GCM-SHA384:ECDHE-ECDSA-AES256-GCM-SHA384:DHE-RSA-AES128-GCM-SHA256:ECDHE-RSA-AES128-SHA256:DHE-RSA-AES128-SHA256:ECDHE-RSA-AES256-SHA384:DHE-RSA-AES256-SHA384:ECDHE-RSA-AES256-SHA256:DHE-RSA-AES256-SHA256:HIGH:!aNULL:!eNULL:!EXPORT:!DES:!RC4:!MD5:!PSK:!SRP:!CAMELLIA"},
  webcrypto,
  subtle,
  createCipheriv: notYet('createCipheriv'),
  createDecipheriv: notYet('createDecipheriv'),
  createSign: notYet('createSign'),
  createVerify: notYet('createVerify'),
  sign: notYet('sign'),
  verify: notYet('verify'),
  generateKeyPair: notYet('generateKeyPair'),
  generateKeyPairSync: notYet('generateKeyPairSync'),
  generateKey: notYet('generateKey'),
  generateKeySync: notYet('generateKeySync'),
  createPublicKey: notYet('createPublicKey'),
  createPrivateKey: notYet('createPrivateKey'),
  createSecretKey: notYet('createSecretKey'),
  publicEncrypt: notYet('publicEncrypt'),
  privateDecrypt: notYet('privateDecrypt'),
  privateEncrypt: notYet('privateEncrypt'),
  publicDecrypt: notYet('publicDecrypt'),
  createDiffieHellman: notYet('createDiffieHellman'),
  createECDH: notYet('createECDH'),
  diffieHellman: notYet('diffieHellman'),
  getDiffieHellman: notYet('getDiffieHellman'),
};
