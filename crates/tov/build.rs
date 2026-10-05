//! Cargo build script: embeds the fetch() library sources (runtime/tls.c and codecs.c with the
//! vendored BoringSSL, brotli, libdeflate, zlib and zstd) in the compiler, so `tov build` can
//! compile them into the archives that fetch() programs link (see src/build.rs), and hashes them
//! once here rather than on every build. It also compiles the vendored libraries, once (cached in
//! OUT_DIR by their sources and flags), into the archive the compiler carries: a machine's first
//! fetch() program then doesn't spend ~12 s compiling them.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};
use std::process::Command;

#[allow(dead_code)]
#[path = "src/tls_flags.rs"]
mod tls_flags;

/// The npm bundler's JavaScript tokenizer: shims are minified here, once.
#[allow(dead_code)]
#[path = "src/npm/lex.rs"]
mod lex;

fn files(dir: &Path, out: &mut Vec<PathBuf>) {
    let mut entries: Vec<PathBuf> = std::fs::read_dir(dir).unwrap().flatten().map(|e| e.path()).collect();
    entries.sort();
    for p in entries {
        if p.is_dir() {
            files(&p, out);
        } else if p.extension().is_some_and(|e| matches!(e.to_str(), Some("c" | "cc" | "h" | "inc" | "S"))) {
            out.push(p);
        }
    }
}

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap();
    let vendor = root.join("vendor");
    let mut list = Vec::new();
    for lib in ["boringssl", "brotli", "libdeflate", "zlib", "zstd"] {
        files(&vendor.join(lib), &mut list);
    }
    list.push(root.join("runtime/tls.c"));
    list.push(root.join("runtime/codecs.c"));
    list.push(root.join("runtime/crypto.c"));
    list.push(root.join("runtime/compress.c"));
    // (one hash for the vendored libraries, one for the runtime's files that use them)
    let mut vendor_parts: Vec<Vec<u8>> = Vec::new();
    let mut runtime_parts: Vec<Vec<u8>> = Vec::new();
    let mut code = String::from("/// (path relative to the repository, contents)\npub static TLS_FILES: &[(&str, &[u8])] = &[\n");
    for p in &list {
        let rel = p.strip_prefix(&root).unwrap().to_string_lossy().replace('\\', "/");
        let _ = writeln!(code, "    ({rel:?}, include_bytes!({:?})),", p.to_string_lossy());
        let parts = if rel.starts_with("vendor/") { &mut vendor_parts } else { &mut runtime_parts };
        parts.push(rel.into_bytes());
        parts.push(std::fs::read(p).unwrap());
    }
    code.push_str("];\n");
    let hash = |parts: &[Vec<u8>]| tls_flags::hash_hex(&parts.iter().map(|p| p.as_slice()).collect::<Vec<_>>());
    let vendor_hash = hash(&vendor_parts);
    let _ = writeln!(code, "/// A hash of TLS_FILES' vendored libraries.\npub const VENDOR_KEY: &str = \"{vendor_hash}\";");
    let _ = writeln!(code, "/// A hash of TLS_FILES' runtime files (runtime/tls.c, ...).\npub const RUNTIME_KEY: &str = \"{}\";", hash(&runtime_parts));
    let out_dir = PathBuf::from(std::env::var("OUT_DIR").unwrap());
    match vendor_archive(&root, &list, &out_dir, &vendor_hash) {
        Some((key, path)) => {
            let _ = writeln!(code, "/// The vendored libraries, compiled with the plain flags (tls_flags), and that archive's key.\npub const VENDOR_ARCHIVE_KEY: &str = \"{key}\";\npub static VENDOR_ARCHIVE: &[u8] = include_bytes!({:?});", path.to_string_lossy());
        }
        None => code.push_str("pub const VENDOR_ARCHIVE_KEY: &str = \"\";\npub static VENDOR_ARCHIVE: &[u8] = &[];\n"),
    }
    let out = out_dir.join("tls_files.rs");
    std::fs::write(out, code).unwrap();
    println!("cargo:rerun-if-changed={}", vendor.display());
    println!("cargo:rerun-if-changed={}", root.join("runtime/tls.c").display());
    println!("cargo:rerun-if-changed={}", root.join("runtime/codecs.c").display());
    println!("cargo:rerun-if-changed={}", root.join("runtime/crypto.c").display());
    println!("cargo:rerun-if-changed={}", root.join("runtime/compress.c").display());
    for p in &list {
        println!("cargo:rerun-if-changed={}", p.display());
    }
    node_shims(&root);
}

/// The vendored libraries compiled into an archive in OUT_DIR, as `tov build` would compile them
/// with the plain flags (no sanitizers) and the compiler it picks (Xcode's clang on macOS): its key
/// and path. None when it can't be (another target, no compiler), and `tov build` compiles them.
fn vendor_archive(root: &Path, list: &[PathBuf], out_dir: &Path, vendor_hash: &str) -> Option<(String, PathBuf)> {
    if std::env::var("TARGET").ok() != std::env::var("HOST").ok() || std::env::var_os("TOV_NO_PREBUILT_VENDOR").is_some() {
        return None;
    }
    let key = tls_flags::vendor_key(vendor_hash, &[]);
    let archive = out_dir.join(format!("vendor-{key}.a"));
    if archive.is_file() {
        return Some((key, archive));
    }
    let xcrun = |args: &[&str]| -> Option<String> {
        let out = Command::new("xcrun").args(args).output().ok()?;
        let s = String::from_utf8(out.stdout).ok()?.trim().to_string();
        (out.status.success() && !s.is_empty()).then_some(s)
    };
    let apple = std::env::var("CARGO_CFG_TARGET_VENDOR").as_deref() == Ok("apple");
    let (cc, cxx, sdk) = if apple {
        (xcrun(&["--find", "clang"])?, xcrun(&["--find", "clang++"])?, xcrun(&["--show-sdk-path"]))
    } else {
        (std::env::var("CC").unwrap_or_else(|_| "cc".into()), std::env::var("CXX").unwrap_or_else(|_| "c++".into()), None)
    };
    let objs_dir = out_dir.join("vendor-objs");
    let _ = std::fs::remove_dir_all(&objs_dir);
    std::fs::create_dir_all(&objs_dir).ok()?;
    let sources: Vec<(&PathBuf, &'static [&'static str])> = list
        .iter()
        .filter(|p| p.starts_with(root.join("vendor")))
        .filter_map(|p| tls_flags::lang(&p.to_string_lossy()).map(|l| (p, l)))
        .collect();
    let includes: Vec<String> = tls_flags::INCLUDES.iter().map(|i| format!("-I{}", root.join(i).display())).collect();
    let next = std::sync::atomic::AtomicUsize::new(0);
    let failed = std::sync::atomic::AtomicBool::new(false);
    let threads = std::thread::available_parallelism().map_or(4, |n| n.get());
    std::thread::scope(|s| {
        for _ in 0..threads {
            s.spawn(|| loop {
                let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                let Some((src, lang)) = sources.get(i) else { break };
                let compiler = if src.extension().is_some_and(|e| e == "cc") { &cxx } else { &cc };
                let mut cmd = Command::new(compiler);
                cmd.args(tls_flags::FLAGS).args(*lang).args(&includes);
                if let Some(sdk) = &sdk {
                    cmd.arg("-isysroot").arg(sdk);
                }
                let ok = cmd.arg("-c").arg(src).arg("-o").arg(objs_dir.join(format!("{i}.o"))).output().is_ok_and(|o| o.status.success());
                if !ok {
                    failed.store(true, std::sync::atomic::Ordering::Relaxed);
                }
            });
        }
    });
    if failed.into_inner() {
        println!("cargo:warning=couldn't precompile the vendored TLS libraries; `tov build` will compile them when a program needs them");
        return None;
    }
    let tmp = out_dir.join("vendor.tmp.a");
    let _ = std::fs::remove_file(&tmp);
    let objs: Vec<PathBuf> = (0..sources.len()).map(|i| objs_dir.join(format!("{i}.o"))).collect();
    let ok = Command::new("ar").arg("rcs").arg(&tmp).args(&objs).output().is_ok_and(|o| o.status.success());
    let _ = std::fs::remove_dir_all(&objs_dir);
    if !ok || std::fs::rename(&tmp, &archive).is_err() {
        return None;
    }
    Some((key, archive))
}

/// runtime/node/**/*.js as a sorted table of (module id, source) for the npm bundler: "path",
/// "internal/util", ... (see src/npm/node_shims.rs).
fn node_shims(root: &Path) {
    fn walk(dir: &Path, out: &mut Vec<PathBuf>) {
        for e in std::fs::read_dir(dir).unwrap().flatten() {
            let p = e.path();
            if p.is_dir() {
                walk(&p, out);
            } else if p.extension().is_some_and(|e| e == "js") {
                out.push(p);
            }
        }
    }
    let dir = root.join("runtime/node");
    let mut files = Vec::new();
    if dir.is_dir() {
        walk(&dir, &mut files);
    }
    let mut entries: Vec<(String, PathBuf)> =
        files.into_iter().map(|p| (p.strip_prefix(&dir).unwrap().with_extension("").to_string_lossy().replace('\\', "/"), p)).collect();
    entries.sort();
    // Each shim is minified (comments and whitespace go; line breaks stay for automatic
    // semicolons), and its static requires found, here rather than in every bundle.
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("node");
    let mut code = String::from("/// (module id, minified source), sorted by id\npub static NODE_SHIMS: &[(&str, &str)] = &[\n");
    let mut requires = String::from("/// The static `require(...)` specifiers of each of NODE_SHIMS\npub static NODE_SHIM_REQUIRES: &[&[&str]] = &[\n");
    for (id, p) in &entries {
        let src = std::fs::read_to_string(p).unwrap();
        let min = lex::minify(&src).unwrap_or_else(|e| panic!("{}: {e}", p.display()));
        let reqs = lex::static_requires(&min).unwrap_or_else(|e| panic!("{}: {e}", p.display()));
        let dest = out.join(format!("{id}.js"));
        std::fs::create_dir_all(dest.parent().unwrap()).unwrap();
        if std::fs::read_to_string(&dest).ok().as_deref() != Some(min.as_str()) {
            std::fs::write(&dest, &min).unwrap();
        }
        let _ = writeln!(code, "    ({id:?}, include_str!({:?})),", dest.to_string_lossy());
        let _ = writeln!(requires, "    &{reqs:?},");
        println!("cargo:rerun-if-changed={}", p.display());
    }
    code.push_str("];\n");
    code.push_str(&requires);
    code.push_str("];\n");
    println!("cargo:rerun-if-changed=src/npm/lex.rs");
    std::fs::write(PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("node_shims.rs"), code).unwrap();
    println!("cargo:rerun-if-changed={}", dir.display());
}
