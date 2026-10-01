//! Bundles npm packages into one script: every module becomes a function in a table, with its
//! static `require(...)` calls resolved at bundle time. The script defines `__barm_npm`, which
//! loads a requested package on first use.

use super::lex::{self, Kind, Tok};
use super::resolve::{Format, Resolver, Target};
use crate::hash::FxMap;
use std::fmt::Write;
use std::path::{Path, PathBuf};

pub struct Bundle {
    pub js: String,
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
}

/// Bundles the packages `specs` imports, resolving them from `root` (the program's directory).
pub fn bundle(root: &Path, specs: &[String]) -> Result<Bundle, String> {
    let resolver = Resolver::default();
    let mut modules: Vec<Module> = Vec::new();
    let mut index: FxMap<Target, usize> = FxMap::default();
    let mut queue: Vec<(usize, Target)> = Vec::new();
    let mut warnings = Vec::new();
    let mut entry_ids = Vec::new();
    for spec in specs {
        let t = resolver.resolve(root, spec)?;
        let id = add(&mut modules, &mut index, &mut queue, t);
        entry_ids.push(id);
    }
    while let Some((id, target)) = queue.pop() {
        let (name, body, requires, dir) = match &target {
            Target::Builtin(b) => {
                let body = match super::node_shims::shim(b) {
                    Some(src) => src.to_string(),
                    None => format!("module.exports = __barm_missing({});", js_string(b)),
                };
                // shims may require other shims ("node:events")
                let reqs = if super::node_shims::shim(b).is_some() { static_requires(&body).map_err(|e| format!("node:{b}: {e}"))? } else { Vec::new() };
                (format!("node:{b}"), body, reqs, root.to_path_buf())
            }
            Target::File(path) => {
                let src = std::fs::read_to_string(path).map_err(|e| format!("can't read {}: {e}", path.display()))?;
                let name = display(path, root);
                let dir = path.parent().unwrap_or(root).to_path_buf();
                match resolver.format(path) {
                    Format::Json => (name, format!("module.exports = {};", src.trim_end().trim_start_matches('\u{feff}')), Vec::new(), dir),
                    Format::CommonJs => {
                        let reqs = static_requires(&src).map_err(|e| format!("{name}: {e}"))?;
                        (name, src, reqs, dir)
                    }
                    Format::Esm => {
                        let (body, reqs) = super::esm::to_commonjs(&src).map_err(|e| format!("{name}: {e}"))?;
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
    }
    let mut js = String::with_capacity(modules.iter().map(|m| m.body.len() + 64).sum::<usize>() + 4096);
    js.push_str(super::node_shims::globals());
    js.push_str(RUNTIME_HEAD);
    js.push_str("var __barm_defs = [\n");
    for m in &modules {
        // `exports` is `this` at a CommonJS module's top level, as in Node.
        let _ = write!(js, "function (module, exports, require, __filename, __dirname) {{\n{}\n}},\n", m.body);
    }
    js.push_str("];\nvar __barm_names = [");
    for m in &modules {
        let _ = write!(js, "{},", js_string(&m.name));
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
    js.push_str(RUNTIME_TAIL);
    Ok(Bundle { js, entries: specs.to_vec(), modules: modules.len(), warnings })
}

fn add(modules: &mut Vec<Module>, index: &mut FxMap<Target, usize>, queue: &mut Vec<(usize, Target)>, t: Target) -> usize {
    if let Some(&id) = index.get(&t) {
        return id;
    }
    let id = modules.len();
    modules.push(Module { name: String::new(), body: String::new(), map: Vec::new() });
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

/// The string literals of `require("...")` calls (not `x.require(...)`), unescaped.
pub fn static_requires(src: &str) -> Result<Vec<String>, String> {
    let toks = lex::tokenize(src).map_err(|e| format!("{} at byte {}", e.message, e.pos))?;
    let mut out = Vec::new();
    for (i, t) in toks.iter().enumerate() {
        if t.kind != Kind::Ident || t.text(src) != "require" {
            continue;
        }
        if i > 0 && is_member_dot(&toks[i - 1], src) {
            continue;
        }
        if let [open, arg, close, ..] = &toks[i + 1..] {
            if open.text(src) == "(" && close.text(src) == ")" && (arg.kind == Kind::Str || arg.kind == Kind::Template) {
                out.push(unquote(arg.text(src)));
            }
        }
    }
    Ok(out)
}

fn is_member_dot(t: &Tok, src: &str) -> bool {
    t.kind == Kind::Punct && (t.text(src) == "." || t.text(src) == "?.")
}

/// The value of a string literal token (quotes and common escapes).
pub fn unquote(lit: &str) -> String {
    let inner = &lit[1..lit.len() - 1];
    if !inner.contains('\\') {
        return inner.to_string();
    }
    let mut out = String::new();
    let mut chars = inner.chars();
    while let Some(c) = chars.next() {
        if c != '\\' {
            out.push(c);
            continue;
        }
        match chars.next() {
            Some('n') => out.push('\n'),
            Some('t') => out.push('\t'),
            Some('r') => out.push('\r'),
            Some('0') => out.push('\0'),
            Some('u') => {
                let hex: String = chars.by_ref().take(4).collect();
                if let Some(ch) = u32::from_str_radix(&hex, 16).ok().and_then(char::from_u32) {
                    out.push(ch);
                }
            }
            Some('x') => {
                let hex: String = chars.by_ref().take(2).collect();
                if let Some(ch) = u32::from_str_radix(&hex, 16).ok().and_then(char::from_u32) {
                    out.push(ch);
                }
            }
            Some('\n') => {}
            Some(other) => out.push(other),
            None => {}
        }
    }
    out
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
const RUNTIME_HEAD: &str = r#"(function () {
var __barm_cache = [];
function __barm_dirname(p) { var i = p.lastIndexOf("/"); return i <= 0 ? "/" : p.slice(0, i); }
function __barm_missing(name) {
  var fail = function () { throw new Error("The Node module \"" + name + "\" is not supported by Barm yet"); };
  return new Proxy({}, { get: function (t, k) { if (k === "__esModule" || typeof k === "symbol" || k === "then") return undefined; return fail; } });
}
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
  __barm_defs[id].call(module.exports, module, module.exports, require, name, __barm_dirname(name));
  module.loaded = true;
  return module.exports;
}
"#;

const RUNTIME_TAIL: &str = r#"var entries = globalThis.__barm_npm;
globalThis.__barm_npm = function (spec) { return __barm_load(entries[spec]); };
})();
"#;
