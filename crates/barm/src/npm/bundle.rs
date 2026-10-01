//! Bundles npm packages into one script: every module becomes a function in a table, with its
//! static `require(...)` calls resolved at bundle time. The script defines `__barm_npm`, which
//! loads a requested package on first use.

pub use super::lex::{static_requires, unquote};
use super::resolve::{Format, Resolver, Target};
use crate::hash::FxMap;
use std::fmt::Write;
use std::path::{Path, PathBuf};

pub struct Bundle {
    /// The loader and the module table: evaluated when the program starts. Modules are compiled
    /// on first `require` from `sources` (see `blob`), or come inline (see `script`).
    pub prelude: String,
    /// Per module: its display name (stack traces show it) and its code wrapped as a function
    /// expression, `(function (module, exports, ...) {` + code + `\n})`, starting on the
    /// code's first line so line numbers match the file's.
    pub sources: Vec<(String, String)>,
    /// Package specifiers the program imports, in the order given.
    pub entries: Vec<String>,
    /// Number of modules (files, JSON and built-ins) in the bundle.
    pub modules: usize,
    pub warnings: Vec<String>,
}

struct Module {
    /// Display name: the path relative to the project, or `node:x`.
    name: String,
    /// The module's code as a function body.
    body: String,
    /// Static require specifier → module index.
    map: Vec<(String, usize)>,
    /// Converted from an ES module (its function takes `esm::ESM_PARAMS`).
    esm: bool,
}

/// Bundles the packages `specs` imports, resolving them from `root` (the program's directory).
pub fn bundle(root: &Path, specs: &[String]) -> Result<Bundle, String> {
    let resolver = Resolver::default();
    let mut modules: Vec<Module> = Vec::new();
    let mut index: FxMap<Target, usize> = FxMap::default();
    let mut queue: Vec<(usize, Target)> = Vec::new();
    let mut warnings = Vec::new();
    let mut entry_ids = Vec::new();
    // Node's globals (`process`, `Buffer`, ...) as a module that runs before the entries
    let globals_id = super::node_shims::shim("__globals").map(|_| add(&mut modules, &mut index, &mut queue, Target::Builtin("internal/bootstrap/globals".into())));
    for spec in specs {
        let t = resolver.resolve(root, spec)?;
        let id = add(&mut modules, &mut index, &mut queue, t);
        entry_ids.push(id);
    }
    while let Some((id, target)) = queue.pop() {
        let mut esm = false;
        let (name, body, requires, dir) = match &target {
            Target::Builtin(b) => {
                let body = match super::node_shims::shim(b) {
                                        Some(src) => src.to_string(),
                    None => format!("module.exports = __barm_missing({});", js_string(b)),
                };
                // shims may require other shims ("node:events")
                let reqs = super::node_shims::shim_requires(b).iter().map(|r| r.to_string()).collect();
                (format!("node:{b}"), body, reqs, root.to_path_buf())
            }
            Target::File(path) => {
                let src = std::fs::read_to_string(path).map_err(|e| format!("can't read {}: {e}", path.display()))?;
                let name = display(path, root);
                let dir = path.parent().unwrap_or(root).to_path_buf();
                let ts = super::resolve::is_typescript(path);
                let jsx = matches!(path.extension().and_then(|e| e.to_str()), Some("tsx" | "jsx"));
                let format = match resolver.format(path) {
                    Format::Detect if ts || jsx || super::esm::has_module_syntax(&src) => Format::Esm,
                    Format::Detect => Format::CommonJs,
                    f => f,
                };
                match format {
                    Format::Json => (name, format!("module.exports = {};", src.trim_end().trim_start_matches('\u{feff}')), Vec::new(), dir),
                    Format::CommonJs | Format::Detect if !ts && !jsx => {
                        let reqs = static_requires(&src).map_err(|e| format!("{name}: {e}"))?;
                        (name, src, reqs, dir)
                    }
                    _ => {
                        let (body, reqs) = super::esm::to_commonjs(&src, ts, jsx).map_err(|e| format!("{name}: {e}"))?;
                        esm = true;
                        (name, body, reqs, dir)
                    }
                }
            }
        };
        let mut map = Vec::with_capacity(requires.len());
        let in_shim = matches!(target, Target::Builtin(_));
        for spec in requires {
            if map.iter().any(|(s, _): &(String, usize)| *s == spec) {
                continue;
            }
            // Inside a shim (Node's own lib/ code) every require names another shim, `internal/...`
            // included; user code can't reach those.
            let resolved = if in_shim {
                let id = spec.strip_prefix("node:").unwrap_or(&spec).to_string();
                if super::node_shims::shim(&id).is_some() || super::resolve::BUILTINS.contains(&id.as_str()) { Ok(Target::Builtin(id)) } else { Err(format!("no shim for \"{spec}\"")) }
            } else {
                resolver.resolve(&dir, &spec)
            };
            match resolved {
                Ok(t) => {
                    let to = add(&mut modules, &mut index, &mut queue, t);
                    map.push((spec, to));
                }
                // Optional dependencies are often required inside try/catch: leave the
                // require to fail at run time, as in Node.
                Err(e) => warnings.push(format!("{name}: {e}")),
            }
        }
        modules[id].name = name;
        modules[id].body = body;
        modules[id].map = map;
        modules[id].esm = esm;
    }
    let mut js = String::with_capacity(4096 + modules.len() * 64);
    if globals_id.is_none() {
        js.push_str(super::node_shims::globals());
    }
    js.push_str(RUNTIME_HEAD);
    let mut sources = Vec::with_capacity(modules.len());
    for m in &mut modules {
        // `exports` is `this` at a CommonJS module's top level, as in Node.
        let params = if m.esm { super::esm::ESM_PARAMS } else { "module, exports, require, __filename, __dirname" };
        let mut body = std::mem::take(&mut m.body);
        if body.starts_with("#!") {
            body.replace_range(0..2, "//");
        }
        sources.push((std::mem::take(&mut m.name), format!("(function ({params}) {{{body}\n}})")));
    }
    js.push_str("var __barm_names = [");
    for (name, _) in &sources {
        let _ = write!(js, "{},", js_string(name));
    }
    js.push_str("];\nvar __barm_maps = [");
    for m in &modules {
        js.push('{');
        for (spec, to) in &m.map {
            let _ = write!(js, "{}:{},", js_string(spec), to);
        }
        js.push_str("},");
    }
    js.push_str("];\nglobalThis.__barm_npm = {");
    for (spec, id) in specs.iter().zip(&entry_ids) {
        let _ = write!(js, "{}:{},", js_string(spec), id);
    }
    js.push_str("};\n");
    if let Some(id) = globals_id {
        let _ = writeln!(js, "__barm_load({id});");
    }
    js.push_str(RUNTIME_TAIL);
    Ok(Bundle { prelude: js, sources, entries: specs.to_vec(), modules: modules.len(), warnings })
}

impl Bundle {
    /// The bundle as one script, every module inline (for engines without Barm's runtime).
    pub fn script(&self) -> String {
        let mut js = String::with_capacity(self.prelude.len() + self.sources.iter().map(|s| s.1.len() + 2).sum::<usize>() + 32);
        js.push_str("var __barm_defs = [\n");
        for (_, src) in &self.sources {
            js.push_str(src);
            js.push_str(",\n");
        }
        js.push_str("];\n");
        js.push_str(&self.prelude);
        js
    }

    /// The bundle as runtime/js.c reads it: a little-endian u32 index — the module count, then
    /// per module the offsets of its name and source and the source's length — then the
    /// prelude and the strings, each NUL-terminated. Offsets count from the blob's start.
    pub fn blob(&self) -> Vec<u8> {
        let n = self.sources.len();
        let header = 4 * (2 + 3 * n);
        let mut data: Vec<u8> = Vec::with_capacity(self.prelude.len() + self.sources.iter().map(|s| s.0.len() + s.1.len() + 2).sum::<usize>() + 1);
        let mut index: Vec<u32> = Vec::with_capacity(2 + 3 * n);
        index.push(n as u32);
        index.push(header as u32); // the prelude
        data.extend_from_slice(self.prelude.as_bytes());
        data.push(0);
        for (name, src) in &self.sources {
            index.push((header + data.len()) as u32);
            data.extend_from_slice(name.as_bytes());
            data.push(0);
            index.push((header + data.len()) as u32);
            data.extend_from_slice(src.as_bytes());
            data.push(0);
            index.push(src.len() as u32);
        }
        let mut out = Vec::with_capacity(header + data.len());
        for v in index {
            out.extend_from_slice(&v.to_le_bytes());
        }
        out.extend_from_slice(&data);
        out
    }
}

fn add(modules: &mut Vec<Module>, index: &mut FxMap<Target, usize>, queue: &mut Vec<(usize, Target)>, t: Target) -> usize {
    if let Some(&id) = index.get(&t) {
        return id;
    }
    let id = modules.len();
    modules.push(Module { name: String::new(), body: String::new(), map: Vec::new(), esm: false });
    index.insert(t.clone(), id);
    queue.push((id, t));
    id
}

fn display(path: &Path, root: &Path) -> String {
    // Keep build-machine paths out of binaries: name modules from their node_modules folder.
    let s = path.to_string_lossy();
    if let Some(i) = s.rfind("/node_modules/") {
        return s[i..].to_string();
    }
    path.strip_prefix(root).map(|p| format!("/{}", p.display())).unwrap_or_else(|_| s.into_owned())
}

/// A JavaScript string literal for `s`.
pub fn js_string(s: &str) -> String {
    let mut out = String::with_capacity(s.len() + 2);
    out.push('"');
    for c in s.chars() {
        match c {
            '"' => out.push_str("\\\""),
            '\\' => out.push_str("\\\\"),
            '\n' => out.push_str("\\n"),
            '\r' => out.push_str("\\r"),
            '\u{2028}' => out.push_str("\\u2028"),
            '\u{2029}' => out.push_str("\\u2029"),
            c if (c as u32) < 0x20 => {
                let _ = write!(out, "\\u{:04x}", c as u32);
            }
            c => out.push(c),
        }
    }
    out.push('"');
    out
}

pub fn root_of(path: &Path) -> PathBuf {
    path.parent().map(Path::to_path_buf).unwrap_or_else(|| PathBuf::from("."))
}

/// The loader: modules are evaluated on first `require`, like Node (cycles see the partial
/// `module.exports`).
const RUNTIME_HEAD: &str = r#"// Helpers the modules call: globals, since each module is compiled on its own.
function __barm_missing(name) {
  var fail = function () { throw new Error("The Node module \"" + name + "\" is not supported by Barm yet"); };
  return new Proxy({}, { get: function (t, k) { if (k === "__esModule" || typeof k === "symbol" || k === "then") return undefined; return fail; } });
}
var __barm_esm_set = new WeakSet();
var __barm_ns_cache = new WeakMap();
function __barm_esm(exports, getters) {
  __barm_esm_set.add(exports);
  Object.defineProperty(exports, "__esModule", { value: true });
  for (var k in getters) Object.defineProperty(exports, k, { get: getters[k], enumerable: true });
}
function __barm_reexport(to, m, k) { Object.defineProperty(to, k, { get: function () { return m[k]; }, enumerable: true }); }
// An ES module's view of a module: its exports if it was an ES module; otherwise (CommonJS, as
// in Node) `default` is `module.exports` and its own keys are named exports.
function __barm_ns(m) {
  if (m === null || (typeof m !== "object" && typeof m !== "function")) return { default: m };
  if (__barm_esm_set.has(m)) return m;
  var ns = __barm_ns_cache.get(m);
  if (ns) return ns;
  ns = {};
  var keys = Object.keys(m);
  for (var i = 0; i < keys.length; i++) if (keys[i] !== "default") __barm_reexport(ns, m, keys[i]);
  Object.defineProperty(ns, "default", { value: m, enumerable: true });
  __barm_ns_cache.set(m, ns);
  return ns;
}
function __barm_star(to, m) {
  var keys = Object.keys(m);
  for (var i = 0; i < keys.length; i++) {
    var k = keys[i];
    if (k !== "default" && k !== "__esModule" && !Object.prototype.hasOwnProperty.call(to, k)) __barm_reexport(to, m, k);
  }
}
(function () {
// (inline modules come first, from `Bundle::script`; otherwise the runtime compiles them)
var __barm_defs = globalThis.__barm_defs || [];
var __barm_compile = globalThis.__barm_compile;
var __barm_cache = [];
function __barm_dirname(p) { var i = p.lastIndexOf("/"); return i <= 0 ? "/" : p.slice(0, i); }
function __barm_load(id) {
  var cached = __barm_cache[id];
  if (cached) return cached.exports;
  var name = __barm_names[id];
  var module = { exports: {}, id: name, filename: name, loaded: false, children: [], paths: [] };
  __barm_cache[id] = module;
  var map = __barm_maps[id];
  var require = function (spec) {
    var to = map[spec];
    if (to === undefined) {
      var e = new Error("Cannot find module '" + spec + "' (from " + name + ")");
      e.code = "MODULE_NOT_FOUND";
      throw e;
    }
    return __barm_load(to);
  };
  require.resolve = function (spec) { var to = map[spec]; if (to === undefined) throw new Error("Cannot find module '" + spec + "'"); return __barm_names[to]; };
  require.cache = {};
  require.main = undefined;
  var def = __barm_defs[id] || (__barm_defs[id] = __barm_compile(id));
  def.call(module.exports, module, module.exports, require, name, __barm_dirname(name));
  module.loaded = true;
  return module.exports;
}
"#;

const RUNTIME_TAIL: &str = r#"var entries = globalThis.__barm_npm;
globalThis.__barm_npm = function (spec) { return __barm_load(entries[spec]); };
})();
"#;
