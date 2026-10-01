//! Cargo build script: embeds the fetch() library sources (runtime/tls.c and codecs.c with the
//! vendored BoringSSL, brotli, libdeflate, zlib and zstd) in the
//! compiler, so `barm build` can compile them into the TLS archive that fetch() programs link
//! (see src/build.rs), and hashes them once here (TLS_KEY) rather than on every build.

use std::fmt::Write as _;
use std::path::{Path, PathBuf};

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
    let (mut a, mut b): (u64, u64) = (0xcbf29ce484222325, 0x84222325cbf29ce4);
    let mut code = String::from("/// (path relative to the repository, contents)\npub static TLS_FILES: &[(&str, &[u8])] = &[\n");
    for p in &list {
        let rel = p.strip_prefix(&root).unwrap().to_string_lossy().replace('\\', "/");
        let _ = writeln!(code, "    ({rel:?}, include_bytes!({:?})),", p.to_string_lossy());
        for &byte in rel.as_bytes().iter().chain(std::fs::read(p).unwrap().iter()) {
            a = (a ^ byte as u64).wrapping_mul(0x100000001b3);
            b = (b.rotate_left(5) ^ byte as u64).wrapping_mul(0x51_7c_c1_b7_27_22_0a_95);
        }
    }
    code.push_str("];\n");
    let _ = writeln!(code, "/// A hash of TLS_FILES (for the archive's cache key).\npub const TLS_KEY: &str = \"{a:016x}{b:016x}\";");
    let out = PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("tls_files.rs");
    std::fs::write(out, code).unwrap();
    println!("cargo:rerun-if-changed={}", vendor.display());
    println!("cargo:rerun-if-changed={}", root.join("runtime/tls.c").display());
    println!("cargo:rerun-if-changed={}", root.join("runtime/codecs.c").display());
    for p in &list {
        println!("cargo:rerun-if-changed={}", p.display());
    }
    node_shims(&root);
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
    let mut code = String::from("/// (module id, source), sorted by id\npub static NODE_SHIMS: &[(&str, &str)] = &[\n");
    for (id, p) in &entries {
        let _ = writeln!(code, "    ({id:?}, include_str!({:?})),", p.to_string_lossy());
        println!("cargo:rerun-if-changed={}", p.display());
    }
    code.push_str("];\n");
    std::fs::write(PathBuf::from(std::env::var("OUT_DIR").unwrap()).join("node_shims.rs"), code).unwrap();
    println!("cargo:rerun-if-changed={}", dir.display());
}
