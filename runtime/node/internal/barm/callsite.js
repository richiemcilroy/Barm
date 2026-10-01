'use strict';

// V8's stack trace API, which packages use to find their callers (depd, callsites,
// source-map-support): when Error.prepareStackTrace is a function, an error's `stack` is what it
// returns for the error and the error's frames as CallSite objects. JavaScriptCore has no such
// hook, so the frames come from parsing its `stack` text ("name@file:line:column"), as Bun does.

// "name@file:line:column", "file:line:column", "name@[native code]", "eval code@"
function parseFrame(line) {
  let name = '';
  let location = line;
  const at = line.indexOf('@');
  if (at >= 0) {
    name = line.slice(0, at);
    location = line.slice(at + 1);
  }
  let file = location;
  let lineNumber = null;
  let column = null;
  const m = /^(.*):(\d+):(\d+)$/.exec(location);
  if (m) {
    file = m[1];
    lineNumber = Number(m[2]);
    column = Number(m[3]);
  }
  const native = location === '[native code]';
  const toplevel = name === 'global code' || name === 'module code' || name === '';
  const isEval = name === 'eval code';
  if (toplevel || isEval) name = null;
  let async = false;
  if (name !== null && name.startsWith('async ')) {
    async = true;
    name = name.slice(6);
  }
  return new CallSite(name, native ? null : file || null, lineNumber, column, { native, toplevel, isEval, async });
}

class CallSite {
  #name;
  #file;
  #line;
  #column;
  #flags;

  constructor(name, file, line, column, flags) {
    this.#name = name;
    this.#file = file;
    this.#line = line;
    this.#column = column;
    this.#flags = flags;
  }

  getThis() { return undefined; }
  getTypeName() { return this.#flags.toplevel ? 'Object' : null; }
  getFunction() { return undefined; }
  getFunctionName() { return this.#name; }
  getMethodName() { return this.#name; }
  getFileName() { return this.#file ?? undefined; }
  getLineNumber() { return this.#line; }
  getColumnNumber() { return this.#column; }
  getEnclosingLineNumber() { return this.#line; }
  getEnclosingColumnNumber() { return this.#column; }
  getEvalOrigin() { return this.#flags.isEval ? 'eval at <anonymous>' : undefined; }
  getScriptNameOrSourceURL() { return this.#file ?? undefined; }
  getScriptHash() { return ''; }
  getPosition() { return 0; }
  getPromiseIndex() { return null; }
  isToplevel() { return this.#flags.toplevel; }
  isEval() { return this.#flags.isEval; }
  isNative() { return this.#flags.native; }
  isConstructor() { return false; }
  isAsync() { return this.#flags.async; }
  isPromiseAll() { return false; }

  toString() {
    const where = this.#flags.native ? 'native' : `${this.#file ?? '<anonymous>'}${this.#line !== null ? `:${this.#line}:${this.#column}` : ''}`;
    const name = this.#name ?? (this.#flags.toplevel ? null : '<anonymous>');
    return `${this.#flags.async ? 'async ' : ''}${name ? `${name} (${where})` : where}`;
  }
}

// the frames of a JavaScriptCore stack, skipping `skip` from the top
function callSites(stack, skip = 0) {
  if (typeof stack !== 'string' || stack === '') return [];
  const lines = stack.split('\n');
  const out = [];
  for (let i = skip; i < lines.length; i++) if (lines[i]) out.push(parseFrame(lines[i]));
  return out;
}

// `stack` as Error.prepareStackTrace makes it for `error`, whose JavaScriptCore stack is `raw`
function prepare(error, raw, skip = 0) {
  const prepareStackTrace = globalThis.Error.prepareStackTrace;
  if (typeof prepareStackTrace !== 'function') return raw;
  return prepareStackTrace(error, callSites(raw, skip));
}

module.exports = { CallSite, callSites, prepare };
