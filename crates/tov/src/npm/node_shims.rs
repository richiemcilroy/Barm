//! Node's built-in modules as JavaScript (runtime/node/*.js), included in a bundle only when
//! something requires them.

include!(concat!(env!("OUT_DIR"), "/node_shims.rs"));

/// The module id a built-in `name` is stored under: `node:` dropped, and "__globals" (what runs
/// before an npm bundle's first module) is internal/bootstrap/globals.
fn shim_id(name: &str) -> &str {
    match name {
        "__globals" => "internal/bootstrap/globals",
        _ => name.strip_prefix("node:").unwrap_or(name),
    }
}

/// The shim for built-in `name` (`"fs"`, `"fs/promises"`, or inside shims `"internal/util"`),
/// if Tov has one. Most are Node.js's own lib/ files (see runtime/node/NODE.md).
pub fn shim(name: &str) -> Option<&'static str> {
    let id = shim_id(name);
    NODE_SHIMS.binary_search_by(|(k, _)| k.cmp(&id)).ok().map(|i| NODE_SHIMS[i].1)
}

/// The static `require(...)` specifiers of shim `name` (found when Tov is built).
pub fn shim_requires(name: &str) -> &'static [&'static str] {
    let id = shim_id(name);
    NODE_SHIMS.binary_search_by(|(k, _)| k.cmp(&id)).ok().map(|i| NODE_SHIM_REQUIRES[i]).unwrap_or(&[])
}

/// Runs before any module: Node's globals (`process`, `Buffer`, `global`, ...). Until the
/// runtime/node shims land this is a minimal stand-in.
pub fn globals() -> &'static str {
    shim("__globals").unwrap_or(MINIMAL_GLOBALS)
}

const MINIMAL_GLOBALS: &str = r#"
if (typeof globalThis.global === "undefined") globalThis.global = globalThis;
if (typeof globalThis.process === "undefined") {
  globalThis.process = {
    env: { NODE_ENV: "production" }, argv: ["tov"], execArgv: [], platform: "darwin", arch: "arm64",
    version: "v22.0.0", versions: { node: "22.0.0" }, release: { name: "node" }, pid: 1, exitCode: undefined,
    cwd: function () { return "/"; }, nextTick: function (fn) { var a = Array.prototype.slice.call(arguments, 1); Promise.resolve().then(function () { fn.apply(null, a); }); },
    emitWarning: function () {}, on: function () { return this; }, once: function () { return this; }, off: function () { return this; },
    removeListener: function () { return this; }, listeners: function () { return []; }, emit: function () { return false; },
    hrtime: Object.assign(function () { var t = Date.now(); return [Math.floor(t / 1000), (t % 1000) * 1e6]; }, { bigint: function () { return BigInt(Date.now()) * 1000000n; } }),
    stdout: { write: function (s) { console.log(String(s).replace(/\n$/, "")); return true; }, isTTY: false },
    stderr: { write: function (s) { console.log(String(s).replace(/\n$/, "")); return true; }, isTTY: false },
    memoryUsage: function () { return { rss: 0, heapTotal: 0, heapUsed: 0, external: 0 }; }, uptime: function () { return 0; },
    features: {}, config: { variables: {} }, binding: function () { throw new Error("process.binding is not supported"); }
  };
}
if (typeof globalThis.queueMicrotask === "undefined") globalThis.queueMicrotask = function (fn) { Promise.resolve().then(fn); };
if (typeof globalThis.setTimeout === "undefined") globalThis.setTimeout = function (fn) { Promise.resolve().then(fn); return 0; };
if (typeof globalThis.clearTimeout === "undefined") globalThis.clearTimeout = function () {};
if (typeof globalThis.setInterval === "undefined") globalThis.setInterval = function () { return 0; };
if (typeof globalThis.clearInterval === "undefined") globalThis.clearInterval = function () {};
if (typeof globalThis.setImmediate === "undefined") globalThis.setImmediate = function (fn) { Promise.resolve().then(fn); return 0; };
"#;
