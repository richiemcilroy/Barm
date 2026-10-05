'use strict';

// internalBinding('types'): Node.js answers these in C++ (V8's own type checks). Here each is
// a brand check: a built-in method that throws unless `this` really is that kind of object,
// so subclasses count and look-alikes (or a forged Symbol.toStringTag) don't.

const { apply } = Reflect;
const getPrototypeOf = Object.getPrototypeOf;
const toString = Object.prototype.toString;

function brandOk(method, value) {
  try {
    apply(method, value, []);
    return true;
  } catch {
    return false;
  }
}

// (a brand check throws for every "no", and a throw costs microseconds: one only for values that
// look like the kind, by prototype or tag, so most answers are quick; a value of the kind from
// another realm with its tag changed is the one that looks otherwise)
function branded(method, Ctor, tag) {
  return (value) => {
    if (value === null || (typeof value !== 'object' && typeof value !== 'function')) return false;
    if (Ctor !== undefined) {
      let looks;
      try {
        looks = value instanceof Ctor || apply(toString, value, []) === tag;
      } catch {
        looks = false;   // (a revoked proxy)
      }
      if (!looks) return false;
    }
    return brandOk(method, value);
  };
}

const typedArrayProto = getPrototypeOf(Uint8Array.prototype);
const typedArrayTag = Object.getOwnPropertyDescriptor(typedArrayProto, Symbol.toStringTag).get;
const byteLengthOf = (C) => Object.getOwnPropertyDescriptor(C.prototype, 'byteLength').get;
const arrayBufferByteLength = byteLengthOf(ArrayBuffer);
const sharedByteLength = typeof SharedArrayBuffer === 'function' ? byteLengthOf(SharedArrayBuffer) : null;
const dataViewByteLength = byteLengthOf(DataView);

function typedArrayKind(value) {
  return apply(typedArrayTag, value, []);
}

const AsyncFunction = getPrototypeOf(async function () {});
const GeneratorFunction = getPrototypeOf(function* () {});
const AsyncGeneratorFunction = getPrototypeOf(async function* () {});
const Generator = getPrototypeOf(function* () {}).prototype;
const AsyncGenerator = getPrototypeOf(async function* () {}).prototype;
const mapIteratorProto = getPrototypeOf(new Map()[Symbol.iterator]());
const setIteratorProto = getPrototypeOf(new Set()[Symbol.iterator]());

function hasProto(value, proto) {
  for (let p = getPrototypeOf(value); p !== null; p = getPrototypeOf(p)) {
    if (p === proto) return true;
  }
  return false;
}

const boxed = (C, tag) => {
  const check = branded(C.prototype.valueOf, C, tag);
  return (v) => typeof v === 'object' && check(v);
};
const isBigIntObject = boxed(BigInt, '[object BigInt]');
const isBooleanObject = boxed(Boolean, '[object Boolean]');
const isNumberObject = boxed(Number, '[object Number]');
const isStringObject = boxed(String, '[object String]');
const isSymbolObject = boxed(Symbol, '[object Symbol]');
const isMap = branded(Object.getOwnPropertyDescriptor(Map.prototype, 'size').get, Map, '[object Map]');
const isSet = branded(Object.getOwnPropertyDescriptor(Set.prototype, 'size').get, Set, '[object Set]');

// ArrayBuffer (1) or SharedArrayBuffer (2), else 0: the engine says which objects are either,
// without a throw (globalThis.__tov_native.typedArrayType: 9), and the likelier getter says which
const typedArrayType = globalThis.__tov_native?.typedArrayType;
function arrayBufferKind(v) {
  if (v === null || (typeof v !== 'object' && typeof v !== 'function')) return 0;
  if (typedArrayType !== undefined && typedArrayType(v) !== 9) return 0;
  const sharedFirst = sharedByteLength !== null && v instanceof SharedArrayBuffer;
  if (brandOk(sharedFirst ? sharedByteLength : arrayBufferByteLength, v)) return sharedFirst ? 2 : 1;
  if (sharedFirst) return brandOk(arrayBufferByteLength, v) ? 1 : 0;
  return sharedByteLength !== null && brandOk(sharedByteLength, v) ? 2 : 0;
}
const isArrayBuffer = (v) => arrayBufferKind(v) === 1;
const isRegExpBranded = branded(Object.getOwnPropertyDescriptor(RegExp.prototype, 'source').get, RegExp, '[object RegExp]');
const isSharedArrayBuffer = (v) => arrayBufferKind(v) === 2;

module.exports = {
  isExternal: () => false,
  isDate: branded(Date.prototype.getTime, Date, '[object Date]'),
  isArgumentsObject: (v) => v !== null && typeof v === 'object' && apply(toString, v, []) === '[object Arguments]',
  isBigIntObject,
  isBooleanObject,
  isNumberObject,
  isStringObject,
  isSymbolObject,
  isBoxedPrimitive: (v) => isNumberObject(v) || isStringObject(v) || isBooleanObject(v) || isBigIntObject(v) || isSymbolObject(v),
  isNativeError(v) {
    if (v === null || typeof v !== 'object') return false;
    // Error.isError, where the engine has it, is exactly this check
    if (typeof Error.isError === 'function') return Error.isError(v);
    return apply(toString, v, []) === '[object Error]' && v instanceof Error;
  },
  // (the source getter throws for anything but a RegExp, without running it)
  isRegExp: (v) => v !== RegExp.prototype && isRegExpBranded(v),
  isAsyncFunction: (v) => typeof v === 'function' && (hasProto(v, AsyncFunction) || hasProto(v, AsyncGeneratorFunction)),
  isGeneratorFunction: (v) => typeof v === 'function' && (hasProto(v, GeneratorFunction) || hasProto(v, AsyncGeneratorFunction)),
  isGeneratorObject: (v) => v !== null && typeof v === 'object' && (hasProto(v, Generator) || hasProto(v, AsyncGenerator)),
  // (no side-effect-free brand check exists for promises: then() would subscribe to it)
  isPromise: (v) => v instanceof Promise && apply(toString, v, []) === '[object Promise]',
  isMap,
  isSet,
  isMapIterator: (v) => v !== null && typeof v === 'object' && getPrototypeOf(v) === mapIteratorProto,
  isSetIterator: (v) => v !== null && typeof v === 'object' && getPrototypeOf(v) === setIteratorProto,
  isWeakMap: branded(WeakMap.prototype.has, WeakMap, '[object WeakMap]'),
  isWeakSet: branded(WeakSet.prototype.has, WeakSet, '[object WeakSet]'),
  isArrayBuffer,
  isDataView: (v) => ArrayBuffer.isView(v) && typedArrayKind(v) === undefined && brandOk(dataViewByteLength, v),
  isSharedArrayBuffer,
  isAnyArrayBuffer: (v) => arrayBufferKind(v) !== 0,
  isProxy: () => false, // not visible from JS
  isModuleNamespaceObject: (v) => v !== null && typeof v === 'object' && getPrototypeOf(v) === null && apply(toString, v, []) === '[object Module]',
  // (the TypedArray checks Node.js does in JS, via the tag getter, are in internal/util/types)
  typedArrayKind,
};
