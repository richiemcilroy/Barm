// Runs a test under a bare JavaScriptCore (the system `jsc` shell) with runtime/node's shims
// behind require(), the way npm bundles load them: `jsc harness.js -- <root> <test.js>`.
// The same test runs under Node (with its own built-ins); the outputs must match.
const [root, testFile, cwd, platform] = arguments;
// (globals.js will give npm bundles the real process object; the shims need only this much)
const stream = { isTTY: false, columns: 80, write: (s) => print(String(s).replace(/\n$/, "")) };
globalThis.process = { platform, cwd: () => cwd, env: {}, versions: {}, argv: [], emitWarning() {}, stdout: stream, stderr: stream,
  nextTick: (fn, ...args) => { Promise.resolve().then(() => fn(...args)); } };
globalThis.queueMicrotask ??= (fn) => { Promise.resolve().then(fn); };
const cache = {};
function requireShim(id) {
  if (id.startsWith("node:")) id = id.slice(5);
  if (cache[id]) return cache[id].exports;
  const module = { exports: {} };
  cache[id] = module;
  const src = readFile(root + "/" + id + ".js");
  const fn = (0, eval)("(function (module, exports, require) {" + src + "\n})");
  fn(module, module.exports, requireShim);
  return module.exports;
}
// console.log formats as Node.js's does: with util.format (the shim's)
globalThis.console = { log: (...a) => print(requireShim("util").format(...a)) };
globalThis.Buffer = requireShim("buffer").Buffer;
// (startup work Node.js does before user code: globals.js will do it in Barm programs)
requireShim("internal/util/debuglog").initializeDebugEnv(process.env.NODE_DEBUG);
// the test runs as a script of its own (so stack traces name its file), with require() a global,
// and its source is readable the way a Barm program's bundle will be (for assert's messages)
globalThis.__barm_source = (file) => (file === testFile ? readFile(testFile) : undefined);
globalThis.require = requireShim;
load(testFile);
