'use strict';

// internalBinding('serdes'): V8's ValueSerializer and ValueDeserializer (the structured clone
// wire format, version 15) behind Node.js's v8.Serializer and v8.Deserializer, written in
// JavaScript. What it writes reads back in Node.js and the other way round: the same tags,
// varints, string encodings and padding, object references and host objects (Node.js's
// DefaultSerializer writes typed arrays as host objects through _writeHostObject).

const kLatestVersion = 15;

const TAG = {
  version: 0xff, padding: 0x00, verifyObjectCount: 0x3f, theHole: 0x2d, undefined: 0x5f, null: 0x30, true: 0x54,
  false: 0x46, int32: 0x49, uint32: 0x55, double: 0x4e, bigint: 0x5a, utf8String: 0x53, oneByteString: 0x22,
  twoByteString: 0x63, objectReference: 0x5e, beginObject: 0x6f, endObject: 0x7b, beginSparseArray: 0x61,
  endSparseArray: 0x40, beginDenseArray: 0x41, endDenseArray: 0x24, date: 0x44, trueObject: 0x79, falseObject: 0x78,
  numberObject: 0x6e, bigintObject: 0x7a, stringObject: 0x73, regexp: 0x52, beginMap: 0x3b, endMap: 0x3a,
  beginSet: 0x27, endSet: 0x2c, arrayBuffer: 0x42, resizableArrayBuffer: 0x7e, arrayBufferTransfer: 0x74,
  arrayBufferView: 0x56, sharedArrayBuffer: 0x75, hostObject: 0x5c, error: 0x72,
};

// typed array subtags
const VIEW_TAGS = [
  [Int8Array, 0x62], [Uint8Array, 0x42], [Uint8ClampedArray, 0x43], [Int16Array, 0x77], [Uint16Array, 0x57],
  [Int32Array, 0x64], [Uint32Array, 0x44], [Float32Array, 0x66], [Float64Array, 0x46], [BigInt64Array, 0x71],
  [BigUint64Array, 0x51], [DataView, 0x3f],
];
if (typeof globalThis.Float16Array === 'function') VIEW_TAGS.push([globalThis.Float16Array, 0x68]);

// RegExp flags as V8 numbers them
const REGEXP_FLAGS = [['g', 1], ['i', 2], ['m', 4], ['y', 8], ['u', 16], ['s', 32], ['d', 128], ['v', 256]];

const ERROR_PROTOS = [[EvalError, 0x45], [RangeError, 0x52], [ReferenceError, 0x46], [SyntaxError, 0x53], [TypeError, 0x54], [URIError, 0x55]];

const toString = Object.prototype.toString;
const tagOf = (v) => toString.call(v).slice(8, -1);

let Buffer;
let inspect;
function describe(value) {
  if (typeof value === 'function') return Function.prototype.toString.call(value);
  if (typeof value === 'symbol') return value.toString();
  return `#<${tagOf(value) === 'Object' ? (Object.getPrototypeOf(value)?.constructor?.name ?? 'Object') : tagOf(value)}>`;
}

class Serializer {
  #buf = new Uint8Array(64);
  #len = 0;
  #ids = new Map();
  #nextId = 0;
  #transfers = new Map();
  #viewsAsHost = false;

  #reserve(n) {
    if (this.#len + n <= this.#buf.length) return;
    let cap = this.#buf.length * 2;
    while (cap < this.#len + n) cap *= 2;
    const next = new Uint8Array(cap);
    next.set(this.#buf.subarray(0, this.#len));
    this.#buf = next;
  }

  #byte(b) {
    this.#reserve(1);
    this.#buf[this.#len++] = b;
  }

  #bytes(u8) {
    this.#reserve(u8.length);
    this.#buf.set(u8, this.#len);
    this.#len += u8.length;
  }

  #varint(n) {
    // (unsigned, up to 2^53)
    do {
      let b = n % 128;
      n = Math.floor(n / 128);
      if (n > 0) b |= 0x80;
      this.#byte(b);
    } while (n > 0);
  }

  #zigzag(n) {
    this.#varint(n >= 0 ? n * 2 : -n * 2 - 1);
  }

  #double(d) {
    this.#reserve(8);
    new DataView(this.#buf.buffer).setFloat64(this.#len, d, true);
    this.#len += 8;
  }

  #fail(value) {
    const message = `${describe(value)} could not be cloned.`;
    throw new (this._getDataCloneError ?? Error)(message);
  }

  #string(s) {
    let oneByte = true;
    for (let i = 0; i < s.length; i++) {
      if (s.charCodeAt(i) > 0xff) {
        oneByte = false;
        break;
      }
    }
    if (oneByte) {
      this.#byte(TAG.oneByteString);
      this.#varint(s.length);
      this.#reserve(s.length);
      for (let i = 0; i < s.length; i++) this.#buf[this.#len++] = s.charCodeAt(i);
      return;
    }
    const byteLength = s.length * 2;
    let varintSize = 1;
    for (let n = byteLength; n >= 128; n = Math.floor(n / 128)) varintSize++;
    // (two-byte strings start at an even offset)
    if ((this.#len + 1 + varintSize) & 1) this.#byte(TAG.padding);
    this.#byte(TAG.twoByteString);
    this.#varint(byteLength);
    this.#reserve(byteLength);
    for (let i = 0; i < s.length; i++) {
      const c = s.charCodeAt(i);
      this.#buf[this.#len++] = c & 0xff;
      this.#buf[this.#len++] = c >> 8;
    }
  }

  #bigint(v) {
    const negative = v < 0n;
    let mag = negative ? -v : v;
    const words = [];
    while (mag > 0n) {
      words.push(mag & 0xffffffffffffffffn);
      mag >>= 64n;
    }
    this.#varint(words.length * 8 * 2 + (negative ? 1 : 0));
    this.#reserve(words.length * 8);
    const dv = new DataView(this.#buf.buffer);
    for (const w of words) {
      dv.setBigUint64(this.#len, w, true);
      this.#len += 8;
    }
  }

  #number(n) {
    if (Number.isInteger(n) && n >= -2147483648 && n <= 2147483647 && !Object.is(n, -0)) {
      this.#byte(TAG.int32);
      this.#zigzag(n);
    } else {
      this.#byte(TAG.double);
      this.#double(n);
    }
  }

  // own enumerable keys, array indices as numbers: how many were written
  #properties(obj, keys) {
    let written = 0;
    for (const key of keys) {
      if (!Object.prototype.propertyIsEnumerable.call(obj, key)) continue;
      const value = obj[key];
      const index = arrayIndex(key);
      if (index >= 0) this.#number(index);
      else this.#string(key);
      this.#value(value);
      written++;
    }
    return written;
  }

  #value(v) {
    switch (typeof v) {
      case 'undefined': return this.#byte(TAG.undefined);
      case 'boolean': return this.#byte(v ? TAG.true : TAG.false);
      case 'number': return this.#number(v);
      case 'bigint':
        this.#byte(TAG.bigint);
        return this.#bigint(v);
      case 'string': return this.#string(v);
      case 'symbol':
      case 'function': return this.#fail(v);
    }
    if (v === null) return this.#byte(TAG.null);
    const id = this.#ids.get(v);
    if (id !== undefined) {
      this.#byte(TAG.objectReference);
      return this.#varint(id);
    }
    const tag = tagOf(v);
    // a view's buffer goes first (or a reference to it), unless views are host objects
    if (ArrayBuffer.isView(v) && !this.#viewsAsHost) {
      this.#value(v.buffer);
      this.#ids.set(v, this.#nextId++);
      return this.#view(v);
    }
    this.#ids.set(v, this.#nextId++);
    this.#receiver(v, tag);
  }

  #view(v) {
    this.#byte(TAG.arrayBufferView);
    let sub = 0x3f;
    for (const [ctor, t] of VIEW_TAGS) {
      if (v instanceof ctor) {
        sub = t;
        break;
      }
    }
    this.#byte(sub);
    this.#varint(v.byteOffset);
    this.#varint(v.byteLength);
    this.#varint(0);
  }

  #receiver(v, tag) {
    if (Array.isArray(v)) return this.#array(v);
    if (ArrayBuffer.isView(v)) {
      this.#byte(TAG.hostObject);
      return this.#host(v);
    }
    switch (tag) {
      case 'Object':
        if (isProxy(v)) return this.#fail(v);
        if (Object.getPrototypeOf(v) === null || isPlainLike(v)) {
          this.#byte(TAG.beginObject);
          const written = this.#properties(v, Object.keys(v));
          this.#byte(TAG.endObject);
          return this.#varint(written);
        }
        return this.#fail(v);
      case 'Date':
        this.#byte(TAG.date);
        return this.#double(v.getTime());
      case 'Boolean':
        return this.#byte(Boolean.prototype.valueOf.call(v) ? TAG.trueObject : TAG.falseObject);
      case 'Number':
        this.#byte(TAG.numberObject);
        return this.#double(Number.prototype.valueOf.call(v));
      case 'BigInt':
        this.#byte(TAG.bigintObject);
        return this.#bigint(BigInt.prototype.valueOf.call(v));
      case 'String':
        this.#byte(TAG.stringObject);
        return this.#string(String.prototype.valueOf.call(v));
      case 'RegExp': {
        this.#byte(TAG.regexp);
        this.#string(v.source);
        let flags = 0;
        for (const [c, bit] of REGEXP_FLAGS) if (v.flags.includes(c)) flags |= bit;
        return this.#varint(flags);
      }
      case 'Map': {
        this.#byte(TAG.beginMap);
        const entries = [...Map.prototype.entries.call(v)];
        for (const [k, val] of entries) {
          this.#value(k);
          this.#value(val);
        }
        this.#byte(TAG.endMap);
        return this.#varint(entries.length * 2);
      }
      case 'Set': {
        this.#byte(TAG.beginSet);
        const values = [...Set.prototype.values.call(v)];
        for (const val of values) this.#value(val);
        this.#byte(TAG.endSet);
        return this.#varint(values.length);
      }
      case 'ArrayBuffer': {
        const transfer = this.#transfers.get(v);
        if (transfer !== undefined) {
          this.#byte(TAG.arrayBufferTransfer);
          return this.#varint(transfer);
        }
        if (v.resizable) {
          this.#byte(TAG.resizableArrayBuffer);
          this.#varint(v.byteLength);
          this.#varint(v.maxByteLength);
        } else {
          this.#byte(TAG.arrayBuffer);
          this.#varint(v.byteLength);
        }
        return this.#bytes(new Uint8Array(v));
      }
      case 'SharedArrayBuffer': {
        if (typeof this._getSharedArrayBufferId !== 'function') return this.#fail(v);
        const sabId = this._getSharedArrayBufferId(v);
        this.#byte(TAG.sharedArrayBuffer);
        return this.#varint(sabId);
      }
      case 'Error': return this.#error(v);
      default:
        return this.#fail(v);
    }
  }

  #array(v) {
    const length = v.length;
    let dense = true;
    for (let i = 0; i < length; i++) {
      if (!Object.prototype.hasOwnProperty.call(v, i)) {
        dense = false;
        break;
      }
    }
    const keys = Object.keys(v);
    if (dense) {
      this.#byte(TAG.beginDenseArray);
      this.#varint(length);
      for (let i = 0; i < length; i++) this.#value(v[i]);
      const written = this.#properties(v, keys.filter((k) => arrayIndex(k) < 0 || arrayIndex(k) >= length));
      this.#byte(TAG.endDenseArray);
      this.#varint(written);
      return this.#varint(length);
    }
    this.#byte(TAG.beginSparseArray);
    this.#varint(length);
    const written = this.#properties(v, keys);
    this.#byte(TAG.endSparseArray);
    this.#varint(written);
    this.#varint(length);
  }

  #error(v) {
    this.#byte(TAG.error);
    const proto = Object.getPrototypeOf(v);
    for (const [ctor, t] of ERROR_PROTOS) {
      if (proto === ctor.prototype) {
        this.#byte(t);
        break;
      }
    }
    const message = Object.getOwnPropertyDescriptor(v, 'message');
    if (message && 'value' in message) {
      this.#byte(0x6d);
      this.#string(`${message.value}`);
    }
    const stack = v.stack;
    if (typeof stack === 'string') {
      this.#byte(0x73);
      this.#string(stack);
    }
    if (Object.prototype.hasOwnProperty.call(v, 'cause')) {
      this.#byte(0x63);
      this.#value(v.cause);
    }
    this.#byte(0x2e);
  }

  #host(v) {
    if (typeof this._writeHostObject !== 'function') return this.#fail(v);
    this._writeHostObject(v);
  }

  writeHeader() {
    this.#byte(TAG.version);
    this.#varint(kLatestVersion);
  }

  writeValue(value) {
    this.#value(value);
    return true;
  }

  releaseBuffer() {
    Buffer ??= require('buffer').Buffer;
    // (its own memory, exactly its size, as Node.js's is)
    const out = Buffer.from(this.#buf.buffer.slice(0, this.#len));
    this.#buf = new Uint8Array(64);
    this.#len = 0;
    return out;
  }

  transferArrayBuffer(id, arrayBuffer) {
    this.#transfers.set(arrayBuffer, id);
  }

  writeUint32(n) {
    this.#varint(n >>> 0);
  }

  writeUint64(hi, lo) {
    this.#varint((hi >>> 0) * 4294967296 + (lo >>> 0));
  }

  writeDouble(d) {
    this.#double(d);
  }

  writeRawBytes(source) {
    if (!ArrayBuffer.isView(source)) {
      const { codes: { ERR_INVALID_ARG_TYPE } } = require('internal/errors');
      throw new ERR_INVALID_ARG_TYPE('source', ['Buffer', 'TypedArray', 'DataView'], source);
    }
    this.#bytes(new Uint8Array(source.buffer, source.byteOffset, source.byteLength));
  }

  _setTreatArrayBufferViewsAsHostObjects(value) {
    this.#viewsAsHost = !!value;
  }
}

function arrayIndex(key) {
  if (typeof key !== 'string' || key === '' || key.length > 10) return -1;
  const n = Number(key);
  return Number.isInteger(n) && n >= 0 && n < 4294967295 && String(n) === key ? n : -1;
}

let getProxyDetails;
function isProxy(v) {
  getProxyDetails ??= require('internal/bindings/util').getProxyDetails ?? (() => undefined);
  return getProxyDetails(v) !== undefined;
}

// plain objects and class instances; not engine objects (Promise, WeakMap, ...)
function isPlainLike(v) {
  for (let p = Object.getPrototypeOf(v); p !== null; p = Object.getPrototypeOf(p)) {
    if (p === Object.prototype) return true;
    const t = tagOf(p);
    if (t !== 'Object') return false;
  }
  return true;
}

class DeserializeError extends Error {}

class Deserializer {
  #b;
  #pos = 0;
  #version = 0;
  #objects = [];
  #transfers = new Map();

  constructor(buffer) {
    if (!ArrayBuffer.isView(buffer)) {
      const { codes: { ERR_INVALID_ARG_TYPE } } = require('internal/errors');
      throw new ERR_INVALID_ARG_TYPE('buffer', ['Buffer', 'TypedArray', 'DataView'], buffer);
    }
    this.buffer = buffer;
    this.#b = new Uint8Array(buffer.buffer, buffer.byteOffset, buffer.byteLength);
  }

  #bad() {
    throw new DeserializeError('Unable to deserialize cloned data.');
  }

  #byte() {
    if (this.#pos >= this.#b.length) this.#bad();
    return this.#b[this.#pos++];
  }

  #peek() {
    while (this.#pos < this.#b.length && this.#b[this.#pos] === TAG.padding) this.#pos++;
    return this.#pos < this.#b.length ? this.#b[this.#pos] : -1;
  }

  #varint() {
    let n = 0;
    let scale = 1;
    for (;;) {
      const b = this.#byte();
      n += (b & 0x7f) * scale;
      if (!(b & 0x80)) return n;
      scale *= 128;
      if (scale > 2 ** 63) this.#bad();
    }
  }

  #zigzag() {
    const n = this.#varint();
    return n % 2 === 0 ? n / 2 : -(n + 1) / 2;
  }

  #double() {
    if (this.#pos + 8 > this.#b.length) this.#bad();
    const d = new DataView(this.#b.buffer, this.#b.byteOffset + this.#pos, 8).getFloat64(0, true);
    this.#pos += 8;
    return d;
  }

  #raw(n) {
    if (this.#pos + n > this.#b.length) this.#bad();
    const out = this.#b.subarray(this.#pos, this.#pos + n);
    this.#pos += n;
    return out;
  }

  #oneByte(n) {
    const bytes = this.#raw(n);
    let s = '';
    for (let i = 0; i < bytes.length; i += 4096) s += String.fromCharCode.apply(null, bytes.subarray(i, i + 4096));
    return s;
  }

  #twoByte(n) {
    if (n % 2) this.#bad();
    const bytes = this.#raw(n);
    let s = '';
    for (let i = 0; i < n; i += 2) s += String.fromCharCode(bytes[i] | (bytes[i + 1] << 8));
    return s;
  }

  #bigint() {
    const bitfield = this.#varint();
    const negative = bitfield & 1;
    const bytes = Math.floor(bitfield / 2);
    const raw = this.#raw(bytes);
    let v = 0n;
    for (let i = bytes - 1; i >= 0; i--) v = (v << 8n) | BigInt(raw[i]);
    return negative ? -v : v;
  }

  #string() {
    const tag = this.#peek();
    this.#pos++;
    if (tag === TAG.oneByteString) return this.#oneByte(this.#varint());
    if (tag === TAG.twoByteString) return this.#twoByte(this.#varint());
    if (tag === TAG.utf8String) return new TextDecoder().decode(this.#raw(this.#varint()));
    this.#bad();
  }

  #add(v) {
    this.#objects.push(v);
    return v;
  }

  // properties until `end`: how many were read
  #properties(obj, end) {
    let n = 0;
    for (;;) {
      if (this.#peek() === end) {
        this.#pos++;
        return n;
      }
      const key = this.#value();
      if (typeof key !== 'string' && typeof key !== 'number') this.#bad();
      obj[key] = this.#value();
      n++;
    }
  }

  #value() {
    const tag = this.#peek();
    if (tag < 0) this.#bad();
    this.#pos++;
    switch (tag) {
      case TAG.verifyObjectCount:
        this.#varint();
        return this.#value();
      case TAG.undefined: return undefined;
      case TAG.null: return null;
      case TAG.true: return true;
      case TAG.false: return false;
      case TAG.int32: return this.#zigzag();
      case TAG.uint32: return this.#varint();
      case TAG.double: return this.#double();
      case TAG.bigint: return this.#bigint();
      case TAG.utf8String: return new TextDecoder().decode(this.#raw(this.#varint()));
      case TAG.oneByteString: return this.#oneByte(this.#varint());
      case TAG.twoByteString: return this.#twoByte(this.#varint());
      case TAG.objectReference: {
        const id = this.#varint();
        if (id >= this.#objects.length) this.#bad();
        const obj = this.#objects[id];
        return obj instanceof ArrayBuffer ? this.#maybeView(obj) : obj;
      }
      case TAG.beginObject: {
        const obj = this.#add({});
        const n = this.#properties(obj, TAG.endObject);
        if (this.#varint() !== n) this.#bad();
        return obj;
      }
      case TAG.beginSparseArray: {
        const length = this.#varint();
        const arr = this.#add(new Array(length));
        const n = this.#properties(arr, TAG.endSparseArray);
        if (this.#varint() !== n || this.#varint() !== length) this.#bad();
        return arr;
      }
      case TAG.beginDenseArray: {
        const length = this.#varint();
        const arr = this.#add(new Array(length));
        for (let i = 0; i < length; i++) {
          if (this.#peek() === TAG.theHole) {
            this.#pos++;
            continue;
          }
          arr[i] = this.#value();
        }
        const n = this.#properties(arr, TAG.endDenseArray);
        if (this.#varint() !== n || this.#varint() !== length) this.#bad();
        return arr;
      }
      case TAG.date: return this.#add(new Date(this.#double()));
      case TAG.trueObject: return this.#add(Object(true));
      case TAG.falseObject: return this.#add(Object(false));
      case TAG.numberObject: return this.#add(Object(this.#double()));
      case TAG.bigintObject: return this.#add(Object(this.#bigint()));
      case TAG.stringObject: return this.#add(Object(this.#string()));
      case TAG.regexp: {
        const source = this.#string();
        const bits = this.#varint();
        let flags = '';
        for (const [c, bit] of REGEXP_FLAGS) if (bits & bit) flags += c;
        try {
          return this.#add(new RegExp(source, flags));
        } catch {
          this.#bad();
        }
        break;
      }
      case TAG.beginMap: {
        const map = this.#add(new Map());
        let n = 0;
        while (this.#peek() !== TAG.endMap) {
          const k = this.#value();
          map.set(k, this.#value());
          n += 2;
        }
        this.#pos++;
        if (this.#varint() !== n) this.#bad();
        return map;
      }
      case TAG.beginSet: {
        const set = this.#add(new Set());
        let n = 0;
        while (this.#peek() !== TAG.endSet) {
          set.add(this.#value());
          n++;
        }
        this.#pos++;
        if (this.#varint() !== n) this.#bad();
        return set;
      }
      case TAG.arrayBuffer: {
        const length = this.#varint();
        const ab = this.#raw(length).slice().buffer;
        this.#add(ab);
        return this.#maybeView(ab);
      }
      case TAG.resizableArrayBuffer: {
        const length = this.#varint();
        const max = this.#varint();
        const ab = new ArrayBuffer(length, { maxByteLength: max });
        new Uint8Array(ab).set(this.#raw(length));
        this.#add(ab);
        return this.#maybeView(ab);
      }
      case TAG.arrayBufferTransfer: {
        const id = this.#varint();
        if (!this.#transfers.has(id)) this.#bad();
        const ab = this.#add(this.#transfers.get(id));
        return this.#maybeView(ab);
      }
      case TAG.arrayBufferView: this.#bad(); break;
      case TAG.hostObject: {
        if (typeof this._readHostObject !== 'function') this.#bad();
        const id = this.#objects.length;
        this.#objects.push(undefined);
        const obj = this._readHostObject();
        this.#objects[id] = obj;
        return obj;
      }
      case TAG.error: return this.#error();
      default: this.#bad();
    }
  }

  #maybeView(ab) {
    if (this.#peek() !== TAG.arrayBufferView) return ab;
    this.#pos++;
    const sub = this.#byte();
    const offset = this.#varint();
    const length = this.#varint();
    if (this.#version >= 14) this.#varint();
    if (offset + length > ab.byteLength) this.#bad();
    if (sub === 0x3f) return this.#add(new DataView(ab, offset, length));
    for (const [ctor, t] of VIEW_TAGS) {
      if (t === sub) {
        if (length % ctor.BYTES_PER_ELEMENT || offset % ctor.BYTES_PER_ELEMENT) this.#bad();
        return this.#add(new ctor(ab, offset, length / ctor.BYTES_PER_ELEMENT));
      }
    }
    this.#bad();
  }

  #error() {
    const id = this.#objects.length;
    this.#objects.push(undefined);
    let Ctor = Error;
    let message;
    let stack;
    let cause;
    let hasCause = false;
    for (;;) {
      const t = this.#byte();
      const proto = ERROR_PROTOS.find(([, code]) => code === t);
      if (proto) Ctor = proto[0];
      else if (t === 0x6d) message = this.#string();
      else if (t === 0x73) stack = this.#string();
      else if (t === 0x63) {
        hasCause = true;
        cause = this.#value();
      } else if (t === 0x2e) break;
      else this.#bad();
    }
    const e = message === undefined ? new Ctor() : new Ctor(message);
    this.#objects[id] = e;
    if (hasCause) Object.defineProperty(e, 'cause', { value: cause, writable: true, configurable: true });
    if (stack !== undefined) Object.defineProperty(e, 'stack', { value: stack, writable: true, configurable: true });
    return e;
  }

  readHeader() {
    if (this.#peek() === TAG.version) {
      this.#pos++;
      this.#version = this.#varint();
      if (this.#version > kLatestVersion) throw new Error('Unable to deserialize cloned data due to invalid or unsupported version.');
      return true;
    }
    throw new Error('Unable to deserialize cloned data due to invalid or unsupported version.');
  }

  readValue() {
    try {
      return this.#value();
    } catch (e) {
      if (e instanceof DeserializeError) throw new Error(e.message);
      throw e;
    }
  }

  getWireFormatVersion() {
    return this.#version;
  }

  transferArrayBuffer(id, arrayBuffer) {
    this.#transfers.set(id, ArrayBuffer.isView(arrayBuffer) ? arrayBuffer.buffer : arrayBuffer);
  }

  readUint32() {
    return this.#varint() >>> 0;
  }

  readUint64() {
    const n = this.#varint();
    return [Math.floor(n / 4294967296) >>> 0, n >>> 0];
  }

  readDouble() {
    return this.#double();
  }

  _readRawBytes(length) {
    const offset = this.#pos;
    this.#raw(length);
    return offset;
  }
}

module.exports = { Serializer, Deserializer };
