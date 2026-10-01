'use strict';

// internalBinding('types'): Node.js answers these in C++ (V8's own type checks). Here each is
// a brand check: a built-in method that throws unless `this` really is that kind of object,
// so subclasses count and look-alikes (or a forged Symbol.toStringTag) don't.

const { apply } = Reflect;
const getPrototypeOf = Object.getPrototypeOf;
const toString = Object.prototype.toString;

function branded(method) {
  return (value) => {
    if (value === null || (typeof value !== 'object' && typeof value !== 'function')) return false;
    try {
      apply(method, value, []);
      return true;
    } catch {
      return false;
    }
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

const boxed = (valueOf) => {
  const check = branded(valueOf);
  return (v) => typeof v === 'object' && check(v);
};
const isBigIntObject = boxed(BigInt.prototype.valueOf);
const isBooleanObject = boxed(Boolean.prototype.valueOf);
const isNumberObject = boxed(Number.prototype.valueOf);
const isStringObject = boxed(String.prototype.valueOf);
const isSymbolObject = boxed(Symbol.prototype.valueOf);
const isMap = branded(Object.getOwnPropertyDescriptor(Map.prototype, 'size').get);
const isSet = branded(Object.getOwnPropertyDescriptor(Set.prototype, 'size').get);
const isArrayBuffer = branded(arrayBufferByteLength);
const isSharedArrayBuffer = sharedByteLength ? branded(sharedByteLength) : () => false;

module.exports = {
  isExternal: () => false,
  isDate: branded(Date.prototype.getTime),
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
  isRegExp: (v) => v !== RegExp.prototype && branded(Object.getOwnPropertyDescriptor(RegExp.prototype, 'source').get)(v),
  isAsyncFunction: (v) => typeof v === 'function' && (hasProto(v, AsyncFunction) || hasProto(v, AsyncGeneratorFunction)),
  isGeneratorFunction: (v) => typeof v === 'function' && (hasProto(v, GeneratorFunction) || hasProto(v, AsyncGeneratorFunction)),
  isGeneratorObject: (v) => v !== null && typeof v === 'object' && (hasProto(v, Generator) || hasProto(v, AsyncGenerator)),
  // (no side-effect-free brand check exists for promises: then() would subscribe to it)
  isPromise: (v) => v instanceof Promise && apply(toString, v, []) === '[object Promise]',
  isMap,
  isSet,
  isMapIterator: (v) => v !== null && typeof v === 'object' && getPrototypeOf(v) === mapIteratorProto,
  isSetIterator: (v) => v !== null && typeof v === 'object' && getPrototypeOf(v) === setIteratorProto,
  isWeakMap: branded(WeakMap.prototype.has),
  isWeakSet: branded(WeakSet.prototype.has),
  isArrayBuffer,
  isDataView: branded(dataViewByteLength),
  isSharedArrayBuffer,
  isAnyArrayBuffer: (v) => isArrayBuffer(v) || isSharedArrayBuffer(v),
  isProxy: () => false, // not visible from JS
  isModuleNamespaceObject: (v) => v !== null && typeof v === 'object' && getPrototypeOf(v) === null && apply(toString, v, []) === '[object Module]',
  // (the TypedArray checks Node.js does in JS, via the tag getter, are in internal/util/types)
  typedArrayKind,
};
