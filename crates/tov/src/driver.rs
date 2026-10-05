//! Finds source files, follows imports, and runs the checker.

use crate::ast::ItemKind;
use crate::check::{self, Module};
use crate::diag::{similar, Applicability, Diagnostic};
use crate::ast::Ast;
use crate::intern::{Interner, Sym};
use crate::parser;
use crate::source::{FileId, SourceFile, SourceMap, Span};
use crate::hash::FxMap as HashMap;
use std::path::{Path, PathBuf};

/// Built-in declarations (the `Error` classes), written in Tov.
const PRELUDE: &str = include_str!("prelude.tov");

/// Standard library modules (Node's names), written in Tov on top of `__native` runtime calls.
/// Standard modules: (name, source, also importable as `"node:name"`/`"name"`).
const STD_MODULES: &[(&str, &str, bool)] =
    &[("fs", include_str!("std/fs.tov"), true), ("path", include_str!("std/path.tov"), true), ("http", include_str!("std/http.tov"), false)];

/// Globals that come from std/http, as in Bun.
pub(crate) const WEB_GLOBALS: &[&str] =
    &["Bun", "Request", "Response", "Headers", "URL", "URLSearchParams", "fetch", "AbortController", "AbortSignal", "DOMException", "FetchError", "ReadableStream", "TextEncoder", "TextDecoder", "Blob", "File", "FormData"];

/// The standard module an import names (`"node:fs"`, `"fs"`, `"std/http"`), if any.
fn std_module(spec: &str) -> Option<&'static (&'static str, &'static str, bool)> {
    if let Some(name) = spec.strip_prefix("std/") {
        return STD_MODULES.iter().find(|m| m.0 == name);
    }
    let name = spec.strip_prefix("node:").unwrap_or(spec);
    STD_MODULES.iter().find(|m| m.0 == name && m.2)
}

pub struct CheckResult {
    pub sm: SourceMap,
    pub diags: Vec<Diagnostic>,
    pub files: usize,
    pub lines: usize,
    /// (load + parse, check) durations.
    pub phases: (std::time::Duration, std::time::Duration),
}

/// Checks the given files and directories (directories are searched for `.tov` files).
/// Diagnostic paths are shown relative to `base`.
pub fn check_paths(paths: &[PathBuf], base: &Path) -> Result<CheckResult, String> {
    check_paths_with(paths, base, None)
}

/// Like `check_paths`, with an explicit checker thread count.
pub fn check_paths_with(paths: &[PathBuf], base: &Path, threads: Option<usize>) -> Result<CheckResult, String> {
    let t0 = std::time::Instant::now();
    let _ = &t0;
    let Loaded { sm, mut interner, modules, mut diags } = load(paths, base)?;
    let t1 = std::time::Instant::now();
    // Profiling aid: TOV_REPEAT_CHECK=n runs the checker n extra times.
    for _ in 0..std::env::var("TOV_REPEAT_CHECK").ok().and_then(|v| v.parse::<u32>().ok()).unwrap_or(0) {
        check::check_program(&modules, &mut interner, &sm, threads);
    }
    diags.extend(check::check_program(&modules, &mut interner, &sm, threads));
    let t2 = std::time::Instant::now();
    diags.sort_by(|a, b| (a.span.file, a.span.start, a.code).cmp(&(b.span.file, b.span.start, b.code)));
    diags.dedup_by(|a, b| a.span == b.span && a.code == b.code && a.message == b.message);
    let lines: usize = sm.files.iter().map(|f| f.line_count()).sum();
    let files = modules.iter().filter(|m| !m.std).count();
    let lines = lines - modules.iter().filter(|m| m.std).map(|m| sm.get(m.file).line_count()).sum::<usize>();
    Ok(CheckResult { files, sm, diags, lines, phases: (t1 - t0, t2 - t1) })
}

/// Parsed program: every reachable module, with parse diagnostics.
pub struct Loaded {
    pub sm: SourceMap,
    pub interner: Interner,
    pub modules: Vec<Module>,
    pub diags: Vec<Diagnostic>,
}

/// Finds, reads and parses the program (following imports).
pub fn load(paths: &[PathBuf], base: &Path) -> Result<Loaded, String> {
    let t0 = std::time::Instant::now();
    let base = normalize(base);
    let mut roots = Vec::new();
    let mut dirs = Vec::new();
    for p in paths {
        if p.is_dir() {
            dirs.push(p.clone());
        } else if p.is_file() {
            roots.push(p.clone());
        } else {
            return Err(format!("no such file or directory: {}", p.display()));
        }
    }
    roots.extend(walk_parallel(dirs));
    let mut roots: Vec<PathBuf> = roots.iter().map(|p| normalize(p)).collect();
    roots.sort();
    roots.dedup();

    // Parse all roots in parallel, each thread with its own interner; merge afterwards.
    let tw = std::time::Instant::now();
    let mut parsed = parse_parallel(&roots, &base)?;
    let tp = std::time::Instant::now();
    // Merge per-thread interners: map each thread's symbols to global ones, then rewrite that
    // thread's ASTs in parallel.
    let mut interner = Interner::default();
    let maps: Vec<Vec<Sym>> = parsed.iter().map(|(local, _)| (0..local.len() as u32).map(|i| interner.intern(local.get(Sym(i)))).collect()).collect();
    std::thread::scope(|scope| {
        for ((_, files), map) in parsed.iter_mut().zip(&maps) {
            scope.spawn(move || {
                for f in files.iter_mut() {
                    f.1.remap_syms(&|s| map[s.0 as usize]);
                }
            });
        }
    });
    let mut sm = SourceMap::default();
    let mut modules: Vec<Module> = Vec::with_capacity(roots.len());
    let mut by_path: HashMap<PathBuf, u32> = HashMap::default();
    let mut diags = Vec::new();
    for (_, files) in parsed {
        for (source, ast, pd) in files {
            let (path, name) = (source.path.clone(), source.name.clone());
            let file = sm.add(source);
            diags.extend(pd);
            by_path.insert(path.clone(), modules.len() as u32);
            modules.push(Module { file, path, name, ast, imports: HashMap::default(), builtin: false, std: false, entry: true, npm: None });
        }
    }

    if std::env::var("TOV_TRACE").is_ok() {
        eprintln!("walk {:?}, parse {:?}, merge {:?}", tw - t0, tp - tw, tp.elapsed());
    }
    // The web globals (`Bun`, `Request`, `Response`, ...) live in std/http: load it when a
    // program mentions one of them.
    if WEB_GLOBALS.iter().any(|n| interner.lookup(n).is_some()) {
        let target = PathBuf::from("<std>/http");
        let src = STD_MODULES.iter().find(|m| m.0 == "http").unwrap().1;
        let file = sm.add(SourceFile::new(target.clone(), "std/http".into(), src.to_string()));
        let (ast, pd) = parser::parse(&sm.get(file).text, file, &mut interner);
        diags.extend(pd);
        by_path.insert(target.clone(), modules.len() as u32);
        modules.push(Module { file, path: target, name: "std/http".into(), ast, imports: HashMap::default(), builtin: false, std: true, entry: false, npm: None });
    }
    // Follow imports (files outside the roots are parsed here, sequentially).
    let mut mi = 0;
    let npm = crate::npm::resolve::Resolver::default();
    while mi < modules.len() {
        let mut targets = Vec::new();
        for (ii, item) in modules[mi].ast.items.iter().enumerate() {
            if let ItemKind::Import(imp) = &item.kind {
                match resolve_import(&modules[mi].path, &imp.path, imp.path_span, &npm) {
                    Ok(target) => targets.push((ii as u32, if target.starts_with("<std>") || target.starts_with("<npm>") { target } else { normalize(&target) })),
                    Err(d) => diags.push(d),
                }
            }
        }
        for (ii, target) in targets {
            let t = match by_path.get(&target) {
                Some(&t) => t,
                None if target.starts_with("<npm>") => {
                    // an npm package: no Tov source; its exports are `Js` values
                    let spec = target.to_string_lossy()["<npm>/".len()..].to_string();
                    let file = sm.add(SourceFile::new(target.clone(), spec.clone(), String::new()));
                    let (ast, pd) = parser::parse("", file, &mut interner);
                    diags.extend(pd);
                    let t = modules.len() as u32;
                    by_path.insert(target.clone(), t);
                    modules.push(Module { file, path: target, name: spec.clone(), ast, imports: HashMap::default(), builtin: false, std: false, entry: false, npm: Some(spec) });
                    t
                }
                None => {
                    let std_src = target.to_str().and_then(|p| p.strip_prefix("<std>/")).and_then(|n| STD_MODULES.iter().find(|m| m.0 == n));
                    let text = match std_src {
                        Some(m) => m.1.to_string(),
                        None => std::fs::read_to_string(&target).map_err(|e| format!("can't read {}: {e}", target.display()))?,
                    };
                    let name = match std_src {
                        Some(m) => format!("{}{}", if m.2 { "node:" } else { "std/" }, m.0),
                        None => display_name(&target, &base),
                    };
                    let file = sm.add(SourceFile::new(target.clone(), name.clone(), text));
                    let (ast, pd) = parser::parse(&sm.get(file).text, file, &mut interner);
                    diags.extend(pd);
                    let t = modules.len() as u32;
                    by_path.insert(target.clone(), t);
                    modules.push(Module { file, path: target, name, ast, imports: HashMap::default(), builtin: false, std: std_src.is_some(), entry: false, npm: None });
                    t
                }
            };
            modules[mi].imports.insert(ii, t);
        }
        mi += 1;
    }
    // The built-in prelude (`Error` and friends), last.
    let file = sm.add(SourceFile::new(PathBuf::from("<builtin>"), "<builtin>".into(), PRELUDE.into()));
    let (ast, pd) = parser::parse(&sm.get(file).text, file, &mut interner);
    diags.extend(pd);
    modules.push(Module { file, path: PathBuf::from("<builtin>"), name: "<builtin>".into(), ast, imports: HashMap::default(), builtin: true, std: true, entry: false, npm: None });

    Ok(Loaded { sm, interner, modules, diags })
}

type Parsed = (SourceFile, Ast, Vec<Diagnostic>);

/// Reads files, then parses them on all cores. File ids are assigned in `roots` order.
fn parse_parallel(roots: &[PathBuf], base: &Path) -> Result<Vec<(Interner, Vec<Parsed>)>, String> {
    let tr = std::time::Instant::now();
    let readers: usize = if roots.len() < 64 { 1 } else { std::env::var("TOV_READERS").ok().and_then(|v| v.parse().ok()).unwrap_or(4) };
    let per = roots.len().div_ceil(readers).max(1);
    let texts: Vec<String> = std::thread::scope(|scope| {
        let handles: Vec<_> = roots.chunks(per).map(|ps| scope.spawn(move || ps.iter().map(|p| std::fs::read_to_string(p).map_err(|e| format!("can't read {}: {e}", p.display()))).collect::<Result<Vec<_>, _>>())).collect();
        handles.into_iter().map(|h| h.join().expect("reader thread panicked")).collect::<Result<Vec<Vec<String>>, String>>()
    })?
    .into_iter()
    .flatten()
    .collect();
    if std::env::var("TOV_TRACE").is_ok() {
        eprintln!("read {:?}", tr.elapsed());
    }
    // Roughly 16 files per parser thread at least; tiny projects parse on one thread.
    let threads = std::thread::available_parallelism().map(|n| n.get()).unwrap_or(1).min(roots.len().div_ceil(16).max(1));
    let chunk = roots.len().div_ceil(threads).max(1);
    let mut work: Vec<Vec<(usize, PathBuf, String)>> = Vec::new();
    for (i, (path, text)) in roots.iter().cloned().zip(texts).enumerate() {
        if i % chunk == 0 {
            work.push(Vec::new());
        }
        work.last_mut().unwrap().push((i, path, text));
    }
    let results = std::thread::scope(|scope| {
        let handles: Vec<_> = work
            .into_iter()
            .map(|files| {
                scope.spawn(move || {
                    let mut interner = Interner::default();
                    let mut out = Vec::with_capacity(files.len());
                    for (i, path, text) in files {
                        let (ast, diags) = parser::parse(&text, FileId(i as u32), &mut interner);
                        let name = display_name(&path, base);
                        out.push((SourceFile::new(path, name, text), ast, diags));
                    }
                    (interner, out)
                })
            })
            .collect();
        handles.into_iter().map(|h| h.join().expect("parser thread panicked")).collect()
    });
    Ok(results)
}

/// Finds `.tov` files under `dirs`, listing one directory level at a time on all cores.
fn walk_parallel(mut level: Vec<PathBuf>) -> Vec<PathBuf> {
    let threads: usize = std::env::var("TOV_WALKERS").ok().and_then(|v| v.parse().ok()).unwrap_or(3);
    let mut files = Vec::new();
    while !level.is_empty() {
        if level.len() < 4 {
            let mut next = Vec::new();
            for d in &level {
                list_dir(d, &mut files, &mut next);
            }
            level = next;
            continue;
        }
        let chunk = level.len().div_ceil(threads).max(1);
        let results: Vec<(Vec<PathBuf>, Vec<PathBuf>)> = std::thread::scope(|scope| {
            let handles: Vec<_> = level
                .chunks(chunk)
                .map(|dirs| {
                    scope.spawn(move || {
                        let (mut files, mut subdirs) = (Vec::new(), Vec::new());
                        for d in dirs {
                            list_dir(d, &mut files, &mut subdirs);
                        }
                        (files, subdirs)
                    })
                })
                .collect();
            handles.into_iter().map(|h| h.join().expect("walker thread panicked")).collect()
        });
        level = Vec::new();
        for (f, d) in results {
            files.extend(f);
            level.extend(d);
        }
    }
    files
}

fn list_dir(dir: &Path, files: &mut Vec<PathBuf>, subdirs: &mut Vec<PathBuf>) {
    let Ok(entries) = std::fs::read_dir(dir) else { return };
    for entry in entries.flatten() {
        let name = entry.file_name();
        let name = name.to_string_lossy();
        if name.starts_with('.') || name == "target" || name == "node_modules" {
            continue;
        }
        let path = entry.path();
        let is_dir = entry.file_type().map(|t| t.is_dir() || (t.is_symlink() && path.is_dir())).unwrap_or(false);
        if is_dir {
            subdirs.push(path);
        } else if name.ends_with(".tov") {
            files.push(path);
        }
    }
}

/// Absolute, with `.` and `..` resolved lexically (no filesystem calls).
fn normalize(path: &Path) -> PathBuf {
    let abs = std::path::absolute(path).unwrap_or_else(|_| path.to_path_buf());
    let mut out = PathBuf::new();
    for c in abs.components() {
        match c {
            std::path::Component::CurDir => {}
            std::path::Component::ParentDir => {
                out.pop();
            }
            other => out.push(other),
        }
    }
    out
}

fn display_name(path: &Path, base: &Path) -> String {
    path.strip_prefix(base).map(|p| p.to_string_lossy().into_owned()).unwrap_or_else(|_| path.to_string_lossy().into_owned())
}

fn resolve_import(from: &Path, spec: &str, span: Span, npm: &crate::npm::resolve::Resolver) -> Result<PathBuf, Diagnostic> {
    if let Some(m) = std_module(spec) {
        return Ok(PathBuf::from(format!("<std>/{}", m.0)));
    }
    if let Some(name) = spec.strip_prefix("std/") {
        let have: Vec<String> = STD_MODULES.iter().map(|(n, _, _)| format!("std/{n}")).collect();
        return Err(Diagnostic::new("N0103", span, format!("there is no standard module \"std/{name}\"")).note("available", have.join(", ")));
    }
    // Node.js's other built-ins come from JavaScript (runtime/node), like npm packages.
    let node = spec.strip_prefix("node:").unwrap_or(spec);
    if crate::npm::resolve::BUILTINS.contains(&node) {
        return Ok(PathBuf::from(format!("<npm>/node:{node}")));
    }
    if spec.starts_with("node:") {
        return Err(Diagnostic::new("N0103", span, format!("\"{spec}\" is not a Node.js built-in module")));
    }
    if !spec.starts_with("./") && !spec.starts_with("../") && !spec.starts_with('/') {
        // an npm package, from the nearest node_modules
        let dir = from.parent().unwrap_or(Path::new("."));
        return match npm.resolve(dir, spec) {
            Ok(_) => Ok(PathBuf::from(format!("<npm>/{spec}"))),
            Err(e) => {
                let pkg = if spec.starts_with('@') { spec.splitn(3, '/').take(2).collect::<Vec<_>>().join("/") } else { spec.split('/').next().unwrap_or(spec).to_string() };
                let mut d = Diagnostic::new("N0104", span, format!("can't find the npm package \"{spec}\""));
                if !e.starts_with("can't find package") {
                    d = d.note("why", e);
                }
                Err(d.note("instead", format!("install it: `bun add {pkg}` (or npm, pnpm, yarn)")))
            }
        };
    }
    let dir = from.parent().unwrap_or(Path::new("."));
    for ext in [".ts", ".js", ".tsx", ".mjs"] {
        if let Some(stem) = spec.strip_suffix(ext) {
            let span_in = Span::new(span.file, span.start, span.end);
            return Err(Diagnostic::new("N0105", span, format!("imports name Tov modules without an extension, found \"{spec}\""))
                .fix(Applicability::Safe, format!("use \"{stem}\""), span_in, format!("\"{stem}\"")));
        }
    }
    let rel = spec.strip_suffix(".tov").unwrap_or(spec);
    let target = dir.join(format!("{rel}.tov"));
    if target.is_file() {
        return Ok(target);
    }
    let index = dir.join(rel).join("index.tov");
    if index.is_file() {
        return Ok(index);
    }
    // Suggest sibling modules.
    let target_dir = target.parent().unwrap_or(dir).to_path_buf();
    let mut siblings = Vec::new();
    if let Ok(entries) = std::fs::read_dir(&target_dir) {
        for e in entries.flatten() {
            let p = e.path();
            if p.extension().map(|x| x == "tov").unwrap_or(false) && p != from
                && let Some(stem) = p.file_stem() {
                    siblings.push(stem.to_string_lossy().into_owned());
                }
        }
    }
    siblings.sort();
    let wanted = Path::new(rel).file_name().map(|f| f.to_string_lossy().into_owned()).unwrap_or_default();
    let prefix = rel.rsplit_once('/').map(|(p, _)| p).unwrap_or(".");
    let mut d = Diagnostic::new("N0101", span, format!("can't find module \"{spec}\""));
    let sug = similar(&wanted, siblings.iter().map(|s| s.as_str()));
    if let Some(first) = sug.first() {
        let fixed = format!("\"{prefix}/{first}\"");
        d = d.note("did you mean", fixed.clone()).fix(Applicability::Maybe, format!("use {fixed}"), span, fixed);
    } else if !siblings.is_empty() {
        d = d.note("modules in that directory", siblings.join(", "));
    }
    Err(d)
}
