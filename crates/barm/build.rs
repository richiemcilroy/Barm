//! Cargo build script: embeds the TLS sources (runtime/tls.c and the vendored mbedTLS) in the
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
        } else if p.extension().is_some_and(|e| e == "c" || e == "h") {
            out.push(p);
        }
    }
}

fn main() {
    let root = Path::new(env!("CARGO_MANIFEST_DIR")).join("../..").canonicalize().unwrap();
    let vendor = root.join("vendor/mbedtls");
    let mut list = Vec::new();
    files(&vendor, &mut list);
    list.push(root.join("runtime/tls.c"));
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
    for p in &list {
        println!("cargo:rerun-if-changed={}", p.display());
    }
}
