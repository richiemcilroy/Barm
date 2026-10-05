//! How the vendored TLS and codec libraries (BoringSSL, brotli, libdeflate, zlib, zstd) are
//! compiled, shared by the crate's build script, which precompiles them into the archive the
//! compiler embeds, and the driver (src/build.rs), which compiles them itself only when that
//! archive doesn't fit the build (another target, or sanitizer flags).

/// Flags for C, assembly and C++ alike (the driver adds the SDK and any sanitizer flags).
pub const FLAGS: &[&str] = &["-O3", "-DNDEBUG", "-w", "-fno-stack-protector", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0", "-ffunction-sections", "-fdata-sections", "-fno-strict-aliasing", "-fvisibility=hidden"];

/// Include directories, relative to the repository.
pub const INCLUDES: &[&str] = &["vendor/boringssl/include", "vendor/brotli/c/include", "vendor/libdeflate", "vendor/zlib", "vendor/zstd/lib", "runtime"];

/// A source file's language flags (`None`: not a source file).
pub fn lang(path: &str) -> Option<&'static [&'static str]> {
    if path.ends_with(".cc") {
        Some(&["-std=c++17", "-fno-exceptions", "-fno-rtti"])
    } else if path.ends_with(".c") {
        Some(&["-std=gnu11"])
    } else if path.ends_with(".S") {
        Some(&[])
    } else {
        None
    }
}

/// The vendored archive's cache key: its sources (their hash) and the flags it's compiled with.
/// Not the compiler's path or the SDK's: the objects link the same whichever clang made them.
pub fn vendor_key(sources_hash: &str, extra_flags: &[&str]) -> String {
    let flags = FLAGS.iter().chain(extra_flags).copied().collect::<Vec<_>>().join(" ");
    hash_hex(&[b"vendor", sources_hash.as_bytes(), flags.as_bytes()])
}

/// A 128-bit content hash (two independent 64-bit lanes), hex-encoded. For cache keys, not security.
pub fn hash_hex(parts: &[&[u8]]) -> String {
    let (mut a, mut b): (u64, u64) = (0xcbf29ce484222325, 0x84222325cbf29ce4);
    for part in parts {
        for &byte in part.iter() {
            a = (a ^ byte as u64).wrapping_mul(0x100000001b3);
            b = (b.rotate_left(5) ^ byte as u64).wrapping_mul(0x51_7c_c1_b7_27_22_0a_95);
        }
        a = (a ^ 0xff).wrapping_mul(0x100000001b3);
        b = (b.rotate_left(5) ^ part.len() as u64).wrapping_mul(0x51_7c_c1_b7_27_22_0a_95);
    }
    format!("{a:016x}{b:016x}")
}
