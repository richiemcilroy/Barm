'use strict';

// internalBinding('contextify'): Node.js's src/node_contextify.cc over Tov's natives
// (globalThis.__tov_native.vm, in runtime/node.c, on JavaScriptCore's C API). Scripts are
// checked when made and run in this context or a contextified one, whose global looks things up
// in its sandbox object first. V8's code cache, timeouts and SIGINT watchdogs have no
// counterpart: cached data is rejected, and a timeout doesn't stop a script.

const { privateSymbols: { contextify_context_private_symbol: kContext } } = require('internal/bindings/util');

const native = globalThis.__tov_native?.vm;

function vm() {
  if (native) return native;
  const e = new Error('vm is not available outside Tov\'s runtime');
  e.code = 'ERR_METHOD_NOT_IMPLEMENTED';
  throw e;
}

// a context object's handle, or undefined for this context
function handleOf(contextified) {
  return contextified === null || contextified === undefined ? undefined : contextified[kContext];
}

let Buffer;

// makeContext(contextObject, name, origin, strings, wasm, microtaskQueue, hostDefinedOptionId)
function makeContext(contextObject) {
  if (typeof contextObject === 'symbol') {
    // vm.constants.DONT_CONTEXTIFY: an ordinary new global
    const handle = vm().makeContext({});
    const global = vm().global(handle);
    Object.defineProperty(global, kContext, { __proto__: null, value: handle, configurable: true });
    return global;
  }
  const handle = vm().makeContext(contextObject);
  Object.defineProperty(contextObject, kContext, { __proto__: null, value: handle, configurable: true });
  return contextObject;
}

class ContextifyScript {
  #code;
  #filename;
  #line;

  // (code, filename, lineOffset, columnOffset, cachedData, produceCachedData, parsingContext)
  constructor(code, filename = 'evalmachine.<anonymous>', lineOffset = 0, columnOffset = 0, cachedData, produceCachedData) {
    this.#code = `${code}`;
    this.#filename = `${filename}`;
    this.#line = lineOffset + 1;
    const error = vm().check(this.#code, this.#filename, this.#line);
    if (error) throw error;
    if (cachedData !== undefined) this.cachedDataRejected = true;
    if (produceCachedData) {
      Buffer ??= require('buffer').Buffer;
      this.cachedDataProduced = false;
    }
    this.sourceMapURL = /\/\/[#@] sourceMappingURL=(\S+)\s*$/.exec(this.#code)?.[1];
  }

  // runInContext(contextifiedObject | null, timeout, displayErrors, breakOnSigint, breakOnFirstLine)
  runInContext(contextified) {
    return vm().run(this.#code, this.#filename, this.#line, handleOf(contextified));
  }

  createCachedData() {
    Buffer ??= require('buffer').Buffer;
    return Buffer.alloc(0);
  }
}

// compileFunction(code, filename, lineOffset, columnOffset, cachedData, produceCachedData,
//   parsingContext, contextExtensions, params, hostDefinedOptionId) -> { function, ... }
function compileFunction(code, filename, lineOffset, columnOffset, cachedData, produceCachedData, parsingContext, contextExtensions, params) {
  const handle = handleOf(parsingContext);
  const names = params ?? [];
  let fn;
  if (contextExtensions?.length) {
    // (the extensions' properties are in scope, through `with` blocks around the function)
    let body = `return function (${names.join(', ')}) {\n${code}\n};`;
    for (let i = contextExtensions.length - 1; i >= 0; i--) body = `with (__tov_extensions[${i}]) { ${body} }`;
    fn = vm().fn(['__tov_extensions'], body, filename, lineOffset, handle)(contextExtensions);
  } else {
    fn = vm().fn(names, `${code}`, filename, lineOffset + 1, handle);
  }
  const result = { function: fn, sourceMapURL: /\/\/[#@] sourceMappingURL=(\S+)\s*$/.exec(code)?.[1] };
  if (cachedData !== undefined) result.cachedDataRejected = true;
  if (produceCachedData) result.cachedDataProduced = false;
  return result;
}

// whether code has ES module syntax (import/export at the top level)
function containsModuleSyntax(code) {
  if (vm().check(`${code}`, 'module', 1) === null) return false;
  return /(^|[\s;])(import\s*[\w{*'"]|export\s|import\.meta)/m.test(code);
}

module.exports = {
  makeContext,
  ContextifyScript,
  compileFunction,
  containsModuleSyntax,
  compileFunctionForCJSLoader(code, filename) {
    const fn = vm().fn(['exports', 'require', 'module', '__filename', '__dirname'], `${code}`, filename, 1);
    return { cachedDataRejected: false, sourceMapURL: undefined, function: fn, canParseAsESM: false };
  },
  startSigintWatchdog() {},
  stopSigintWatchdog: () => false,
  watchdogHasPendingSigint: () => false,
  measureMemory: () => Promise.resolve({ total: { jsMemoryEstimate: 0, jsMemoryRange: [0, 0] } }),
  constants: { measureMemory: { mode: { SUMMARY: 0, DETAILED: 1 }, execution: { DEFAULT: 0, EAGER: 1 } } },
};
