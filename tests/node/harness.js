// Runs a test under a bare JavaScriptCore (the system `jsc` shell) with runtime/node's shims
// behind require(), the way npm bundles load them: `jsc harness.js -- <root> <test.js> <cwd> <platform>`.
// The same test runs under Node.js (with its own built-ins); the outputs must match.
const [root, testFile, cwd, platform] = arguments;
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

// The natives a Barm program gets from runtime/node.c, from what the jsc shell has (its own
// setTimeout, before Node.js's replaces it)
const jscSetTimeout = setTimeout;
const start = preciseTime();
const pending = { 1: "", 2: "" };
let onTimer = null, onCheck = null, timerGeneration = 0;
globalThis.__barm_native = {
  info: () => ({ argv: ["barm", testFile], execArgv: [], execPath: "/usr/local/bin/barm", pid: 4242, ppid: 1, platform, arch: "arm64", env: {}, title: "barm" }),
  cwd: () => cwd,
  chdir() {},
  exit(code) { throw new Error(`exit(${code})`); },
  umask: () => 0o22,
  hrtime: () => (preciseTime() - start) * 1e9,
  // whole lines go to print (it ends each with a newline)
  write(fd, data) {
    pending[fd] += typeof data === "string" ? data : String.fromCharCode(...data);
    const lines = pending[fd].split("\n");
    pending[fd] = lines.pop();
    for (const line of lines) print(line);
    return data.length;
  },
  isatty: () => false,
  windowSize: () => [80, 24],
  memoryUsage: () => ({ rss: 1, heapTotal: 1, heapUsed: 1, external: 0, arrayBuffers: 0 }),
  cpuUsage: () => [0, 0],
  ids: () => [501, 20, 501, 20],
  kill() {},
  now: () => Math.floor((preciseTime() - start) * 1000),
  timerSetup(timer, check) { onTimer = timer; onCheck = check; },
  timerSchedule(ms) {
    const gen = ++timerGeneration;
    jscSetTimeout(() => { if (gen === timerGeneration) onTimer(); }, ms);
  },
  timerRef() {},
  requestCheck() { jscSetTimeout(() => onCheck(), 0); },
};

// what a Barm program has before its first module runs
requireShim("internal/bootstrap/globals");
// console.log formats as Node.js's does: with util.format (the shim's)
globalThis.console = { log: (...a) => print(requireShim("util").format(...a)) };

// the test runs as a script of its own (so stack traces name its file), with require() a global,
// and its source is readable the way a Barm program's bundle will be (for assert's messages)
globalThis.__barm_source = (file) => (file === testFile ? readFile(testFile) : undefined);
globalThis.require = requireShim;
load(testFile);
