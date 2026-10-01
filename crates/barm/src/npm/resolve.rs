//! Node's module resolution: relative files, packages in `node_modules` (`exports` with
//! conditions and `*` patterns, `main`, index files), `#imports`, and built-in modules.

use super::json::{self, Json};
use crate::hash::FxMap;
use std::cell::RefCell;
use std::path::{Path, PathBuf};

/// Node's built-in modules (by name, without `node:`).
pub const BUILTINS: &[&str] = &[
    "assert", "assert/strict", "async_hooks", "buffer", "child_process", "cluster", "console", "constants", "crypto", "dgram",
    "diagnostics_channel", "dns", "dns/promises", "domain", "events", "fs", "fs/promises", "http", "http2", "https", "inspector",
    "module", "net", "os", "path", "path/posix", "path/win32", "perf_hooks", "process", "punycode", "querystring", "readline",
    "readline/promises", "repl", "stream", "stream/promises", "stream/web", "stream/consumers", "string_decoder", "sys", "timers",
    "timers/promises", "tls", "trace_events", "tty", "url", "util", "util/types", "v8", "vm", "wasi", "worker_threads", "zlib",
    "test", "sqlite",
];

#[derive(Clone, Debug, PartialEq, Eq, Hash)]
pub enum Target {
    File(PathBuf),
    /// A Node built-in, by name without `node:` (`"fs"`, `"fs/promises"`).
    Builtin(String),
}

#[derive(Clone, Copy, Debug, PartialEq, Eq)]
pub enum Format {
    CommonJs,
    Esm,
    /// `.js` in a package without a `type`: CommonJS unless it has ES module syntax (Node's
    /// module syntax detection).
    Detect,
    Json,
}

/// TypeScript source (types are stripped when bundling).
pub fn is_typescript(file: &Path) -> bool {
    matches!(file.extension().and_then(|e| e.to_str()), Some("ts" | "tsx" | "mts" | "cts"))
}

pub struct Resolver {
    /// package.json contents by directory (None: no package.json there).
    packages: RefCell<FxMap<PathBuf, Option<std::rc::Rc<Json>>>>,
}

/// `exports` conditions, most preferred first: CommonJS builds, then ES modules.
const REQUIRE_CONDITIONS: &[&str] = &["require", "node", "default"];
const IMPORT_CONDITIONS: &[&str] = &["import", "node", "default"];

impl Default for Resolver {
    fn default() -> Self {
        Resolver { packages: RefCell::new(FxMap::default()) }
    }
}

impl Resolver {
    /// Resolves `spec` as required from a file in `from_dir`.
    pub fn resolve(&self, from_dir: &Path, spec: &str) -> Result<Target, String> {
        if let Some(name) = spec.strip_prefix("node:") {
            return Ok(Target::Builtin(name.to_string()));
        }
        if BUILTINS.contains(&spec) {
            return Ok(Target::Builtin(spec.to_string()));
        }
        if spec.starts_with("./") || spec.starts_with("../") || spec == "." || spec == ".." || spec.starts_with('/') {
            let p = if spec.starts_with('/') { PathBuf::from(spec) } else { from_dir.join(spec) };
            return self.file_or_dir(&p).map(Target::File).ok_or_else(|| format!("can't find \"{spec}\" from {}", from_dir.display()));
        }
        if spec.starts_with('#') {
            return self.package_imports(from_dir, spec);
        }
        self.package(from_dir, spec)
    }

    /// The format of a resolved file: by extension, then the nearest package.json `type`.
    pub fn format(&self, file: &Path) -> Format {
        match file.extension().and_then(|e| e.to_str()) {
            Some("json") => Format::Json,
            Some("mjs") | Some("mts") => Format::Esm,
            Some("cjs") | Some("cts") => Format::CommonJs,
            ext => {
                let mut dir = file.parent();
                while let Some(d) = dir {
                    if let Some(pkg) = self.package_json(d) {
                        return match pkg.get("type").and_then(|t| t.as_str()) {
                            Some("module") => Format::Esm,
                            Some("commonjs") => Format::CommonJs,
                            _ if matches!(ext, Some("ts" | "tsx")) => Format::Esm,
                            _ => Format::Detect,
                        };
                    }
                    dir = d.parent();
                }
                Format::Detect
            }
        }
    }

    fn package_json(&self, dir: &Path) -> Option<std::rc::Rc<Json>> {
        if let Some(p) = self.packages.borrow().get(dir) {
            return p.clone();
        }
        let p = std::fs::read_to_string(dir.join("package.json")).ok().and_then(|t| json::parse(&t).ok()).map(std::rc::Rc::new);
        self.packages.borrow_mut().insert(dir.to_path_buf(), p.clone());
        p
    }

    fn package(&self, from_dir: &Path, spec: &str) -> Result<Target, String> {
        // `@scope/name/sub` or `name/sub`
        let mut parts = spec.splitn(3, '/');
        let first = parts.next().unwrap_or("");
        let (name, rest) = if first.starts_with('@') {
            let second = parts.next().ok_or_else(|| format!("invalid package name \"{spec}\""))?;
            (format!("{first}/{second}"), parts.next())
        } else {
            let rest = spec.split_once('/').map(|(_, r)| r);
            (first.to_string(), rest)
        };
        let subpath = match rest {
            Some(r) => format!("./{r}"),
            None => ".".to_string(),
        };
        let mut dir = Some(from_dir);
        while let Some(d) = dir {
            if d.file_name().is_some_and(|f| f == "node_modules") {
                dir = d.parent();
                continue;
            }
            let pkg_dir = d.join("node_modules").join(&name);
            if pkg_dir.is_dir() {
                return self.in_package(&pkg_dir, &subpath, spec);
            }
            dir = d.parent();
        }
        Err(format!("can't find package \"{name}\" (install it, e.g. `bun add {name}`)"))
    }

    fn in_package(&self, pkg_dir: &Path, subpath: &str, spec: &str) -> Result<Target, String> {
        let pkg = self.package_json(pkg_dir);
        if let Some(exports) = pkg.as_ref().and_then(|p| p.get("exports")) {
            for conds in [REQUIRE_CONDITIONS, IMPORT_CONDITIONS] {
                match resolve_exports(exports, subpath, conds) {
                    Some(Some(target)) => {
                        let p = pkg_dir.join(target.trim_start_matches("./"));
                        return self.exact_file(&p).map(Target::File).ok_or_else(|| format!("\"{spec}\" resolves to {}, which doesn't exist", p.display()));
                    }
                    Some(None) => return Err(format!("\"{spec}\" is not exported by its package (package.json `exports`)")),
                    None => {}
                }
            }
            return Err(format!("\"{spec}\" is not exported by its package (package.json `exports`)"));
        }
        if subpath == "." {
            let main = pkg.as_ref().and_then(|p| p.get("main")).and_then(|m| m.as_str()).unwrap_or("index.js").to_string();
            if let Some(f) = self.file_or_dir(&pkg_dir.join(&main)) {
                return Ok(Target::File(f));
            }
            return self.file_or_dir(&pkg_dir.join("index.js")).map(Target::File).ok_or_else(|| format!("can't find the entry point of \"{spec}\""));
        }
        self.file_or_dir(&pkg_dir.join(&subpath[2..])).map(Target::File).ok_or_else(|| format!("can't find \"{spec}\""))
    }

    fn package_imports(&self, from_dir: &Path, spec: &str) -> Result<Target, String> {
        let mut dir = Some(from_dir);
        while let Some(d) = dir {
            if let Some(pkg) = self.package_json(d) {
                let imports = pkg.get("imports").ok_or_else(|| format!("\"{spec}\": the package has no `imports` field"))?;
                for conds in [REQUIRE_CONDITIONS, IMPORT_CONDITIONS] {
                    if let Some(Some(target)) = resolve_map(imports, spec, conds) {
                        if target.starts_with("./") {
                            let p = d.join(&target[2..]);
                            return self.exact_file(&p).map(Target::File).ok_or_else(|| format!("\"{spec}\" resolves to {}, which doesn't exist", p.display()));
                        }
                        return self.resolve(d, &target);
                    }
                }
                return Err(format!("\"{spec}\" is not in the package's `imports`"));
            }
            dir = d.parent();
        }
        Err(format!("\"{spec}\": no package.json with `imports` found"))
    }

    fn exact_file(&self, p: &Path) -> Option<PathBuf> {
        if p.is_file() {
            return canonical(p);
        }
        self.file_or_dir(p)
    }

    /// `p`, `p.js`, `p.json`, `p.cjs`, `p.mjs`, then the TypeScript extensions, then `p/` as a
    /// directory (package.json `main`, then `index.*`). TypeScript's ES module convention names
    /// `x.ts` as `x.js` in imports: that is tried too.
    fn file_or_dir(&self, p: &Path) -> Option<PathBuf> {
        if p.is_file() {
            return canonical(p);
        }
        let s = p.as_os_str().to_string_lossy();
        for (js, ts) in [(".js", [".ts", ".tsx"]), (".mjs", [".mts", ".mts"]), (".cjs", [".cts", ".cts"]), (".jsx", [".tsx", ".tsx"])] {
            if let Some(stem) = s.strip_suffix(js) {
                for ext in ts {
                    let f = PathBuf::from(format!("{stem}{ext}"));
                    if f.is_file() {
                        return canonical(&f);
                    }
                }
            }
        }
        for ext in [".js", ".json", ".cjs", ".mjs", ".ts", ".tsx", ".mts", ".cts", ".jsx"] {
            let f = PathBuf::from(format!("{s}{ext}"));
            if f.is_file() {
                return canonical(&f);
            }
        }
        if p.is_dir() {
            if let Some(main) = self.package_json(p).and_then(|pkg| pkg.get("main").and_then(|m| m.as_str()).map(String::from))
                && let Some(f) = self.file_or_dir(&p.join(main))
            {
                return Some(f);
            }
            for idx in ["index.js", "index.json", "index.cjs", "index.mjs", "index.ts", "index.tsx", "index.jsx"] {
                let f = p.join(idx);
                if f.is_file() {
                    return canonical(&f);
                }
            }
        }
        None
    }
}

fn canonical(p: &Path) -> Option<PathBuf> {
    std::fs::canonicalize(p).ok()
}

/// Node's PACKAGE_EXPORTS_RESOLVE for `subpath` (`"."` or `"./x"`): `None` if nothing
/// matched these conditions, `Some(None)` if the subpath is explicitly not exported.
fn resolve_exports(exports: &Json, subpath: &str, conds: &[&str]) -> Option<Option<String>> {
    let is_subpath_map = matches!(exports, Json::Object(entries) if entries.first().is_some_and(|(k, _)| k.starts_with('.')));
    if !is_subpath_map {
        // sugar: `"exports": "./x.js"` or a conditions object for "."
        if subpath != "." {
            return Some(None);
        }
        return resolve_target(exports, "", conds);
    }
    resolve_map(exports, subpath, conds)
}

/// Looks `key` up in an exports/imports map: exact keys first, then the longest `*` pattern.
fn resolve_map(map: &Json, key: &str, conds: &[&str]) -> Option<Option<String>> {
    let Json::Object(entries) = map else { return None };
    if let Some((_, target)) = entries.iter().find(|(k, _)| k == key && !k.contains('*')) {
        return resolve_target(target, "", conds);
    }
    let mut best: Option<(&str, &Json, String)> = None;
    for (k, target) in entries {
        let Some(star) = k.find('*') else { continue };
        let (prefix, suffix) = (&k[..star], &k[star + 1..]);
        if key.len() >= prefix.len() + suffix.len() && key.starts_with(prefix) && key.ends_with(suffix) {
            let matched = key[prefix.len()..key.len() - suffix.len()].to_string();
            if best.as_ref().is_none_or(|(bk, _, _)| prefix.len() > bk.find('*').unwrap_or(0)) {
                best = Some((k.as_str(), target, matched));
            }
        }
    }
    // A trailing-slash folder mapping (deprecated but still seen): `"./sub/": "./dist/sub/"`.
    if best.is_none()
        && let Some((k, target)) = entries.iter().filter(|(k, _)| k.ends_with('/') && key.starts_with(k.as_str())).max_by_key(|(k, _)| k.len())
    {
        let rest = key[k.len()..].to_string();
        return resolve_target(target, &rest, conds).map(|t| t.map(|t| format!("{t}{rest}").replace(&format!("{rest}{rest}"), &rest)));
    }
    let (_, target, matched) = best?;
    resolve_target(target, &matched, conds)
}

fn resolve_target(target: &Json, star: &str, conds: &[&str]) -> Option<Option<String>> {
    match target {
        Json::Null => Some(None),
        Json::Str(s) => Some(Some(s.replace('*', star))),
        Json::Array(items) => {
            for it in items {
                if let Some(Some(t)) = resolve_target(it, star, conds) {
                    return Some(Some(t));
                }
            }
            None
        }
        Json::Object(entries) => {
            for (k, v) in entries {
                if k == "default" || conds.contains(&k.as_str()) {
                    if let Some(r) = resolve_target(v, star, conds) {
                        return Some(r);
                    }
                }
            }
            None
        }
        _ => None,
    }
}
