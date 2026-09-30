//! `barm build` / `run` / `test`: check, generate C, compile with the system C compiler.
//!
//! Binaries are cached by a hash of the generated C and the compiler flags, in one per-user
//! directory shared by every checkout and worktree (`$BARM_CACHE_DIR`, else
//! `$XDG_CACHE_HOME/barm`, else `~/.cache/barm`).

use crate::check;
use crate::codegen::{self, Mode};
use crate::diag::Diagnostic;
use crate::driver;
use crate::source::SourceMap;
use std::path::{Path, PathBuf};
use std::process::Command;
use std::time::{Duration, Instant};

/// runtime/tls.c and the vendored BoringSSL, embedded by the Cargo build script (../build.rs).
mod tls_files {
    include!(concat!(env!("OUT_DIR"), "/tls_files.rs"));
}

pub enum BuildError {
    Diagnostics(SourceMap, Vec<Diagnostic>),
    Message(String),
}

pub struct Built {
    pub binary: PathBuf,
    /// It links TLS (the program calls fetch()).
    pub tls: bool,
    pub cached: bool,
    /// The program's source files on disk (the entry and everything it imports).
    pub sources: Vec<PathBuf>,
    /// (check, generate C, C compile)
    pub timings: (Duration, Duration, Duration),
}

pub struct Options {
    pub mode: Mode,
    /// Integer overflow wraps instead of trapping (like Rust release builds).
    pub unchecked: bool,
    /// C optimization flag.
    pub opt: String,
    /// Also write the generated C here.
    pub emit_c: Option<PathBuf>,
    /// Keep function names and C debug info (`barm build -g`) for debuggers and profilers;
    /// otherwise local symbols are stripped (smaller binaries).
    pub symbols: bool,
}

/// The files on disk behind a source map (not the standard library's built-in modules).
pub fn source_paths(sm: &SourceMap) -> Vec<PathBuf> {
    sm.files.iter().map(|f| f.path.clone()).filter(|p| p.is_file()).collect()
}

pub fn cache_dir() -> PathBuf {
    if let Ok(d) = std::env::var("BARM_CACHE_DIR") {
        return PathBuf::from(d);
    }
    if let Ok(d) = std::env::var("XDG_CACHE_HOME") {
        return PathBuf::from(d).join("barm");
    }
    let home = std::env::var("HOME").unwrap_or_else(|_| ".".into());
    PathBuf::from(home).join(".cache").join("barm")
}

/// The C compiler: `$CC`, else the newest clang we can find (Homebrew LLVM is usually newer
/// than the system compiler, and generates faster code), else `cc`. On macOS, with the SDK to
/// pass as `-isysroot`.
fn c_compiler() -> (String, Option<String>) {
    if let Ok(cc) = std::env::var("CC") {
        return (cc, None);
    }
    // macOS: the system clang generates code as fast as Homebrew's LLVM (bench/run.py) and
    // starts faster, and it links in the same invocation. Called directly rather than through
    // /usr/bin/cc, an xcrun trampoline that adds 10–15 ms to every build.
    if cfg!(target_vendor = "apple") {
        if let Some((clang, sdk)) = apple_toolchain() {
            return (clang, Some(sdk));
        }
        if Path::new("/usr/bin/cc").is_file() {
            return ("/usr/bin/cc".into(), None);
        }
    }
    for candidate in ["/opt/homebrew/opt/llvm/bin/clang", "/usr/local/opt/llvm/bin/clang", "/usr/lib/llvm-21/bin/clang", "/usr/lib/llvm-20/bin/clang"] {
        if Path::new(candidate).is_file() {
            return (candidate.to_string(), None);
        }
    }
    ("cc".into(), None)
}

/// Xcode's clang and the macOS SDK, found with xcrun once and remembered in the cache.
fn apple_toolchain() -> Option<(String, String)> {
    let file = cache_dir().join("toolchain");
    if let Ok(text) = std::fs::read_to_string(&file) {
        let mut lines = text.lines();
        if let (Some(clang), Some(sdk)) = (lines.next(), lines.next())
            && Path::new(clang).is_file()
            && Path::new(sdk).is_dir()
        {
            return Some((clang.to_string(), sdk.to_string()));
        }
    }
    let run = |args: &[&str]| -> Option<String> {
        let out = Command::new("xcrun").args(args).output().ok()?;
        let s = String::from_utf8(out.stdout).ok()?.trim().to_string();
        (out.status.success() && !s.is_empty()).then_some(s)
    };
    let clang = run(&["--find", "clang"])?;
    let sdk = run(&["--show-sdk-path"])?;
    if !Path::new(&clang).is_file() || !Path::new(&sdk).is_dir() {
        return None;
    }
    let _ = std::fs::create_dir_all(cache_dir());
    let tmp = file.with_extension(format!("tmp{}", std::process::id()));
    if std::fs::write(&tmp, format!("{clang}\n{sdk}\n")).is_ok() {
        let _ = std::fs::rename(&tmp, &file);
    }
    Some((clang, sdk))
}

/// The linker driver. Homebrew clang loads its LTO library on every link (~40 ms), so when we
/// picked it ourselves on macOS, the system driver links instead (same `ld`, same objects).
fn linker(cc: &str) -> String {
    if std::env::var_os("CC").is_none() && cfg!(target_vendor = "apple") && cc.contains("/opt/llvm/") && Path::new("/usr/bin/cc").is_file() {
        return "/usr/bin/cc".into();
    }
    cc.to_string()
}

pub fn build(paths: &[PathBuf], base: &Path, opts: &Options) -> Result<Built, BuildError> {
    let t0 = Instant::now();
    let loaded = driver::load(paths, base).map_err(BuildError::Message)?;
    let driver::Loaded { sm, mut interner, modules, diags } = loaded;
    let sources = source_paths(&sm);
    if !diags.is_empty() {
        return Err(BuildError::Diagnostics(sm, diags));
    }
    let (diags, mut checker) = check::check_for_build(&modules, &mut interner, &sm);
    if !diags.is_empty() {
        drop(checker);
        return Err(BuildError::Diagnostics(sm, diags));
    }
    let t1 = Instant::now();
    let program = match codegen::generate(&mut checker, opts.mode) {
        Ok(p) => p,
        Err(d) => {
            drop(checker);
            return Err(BuildError::Diagnostics(sm, d));
        }
    };
    drop(checker);
    let codegen::Program { c: c_src, uses_tls } = program;
    let t2 = Instant::now();
    if let Some(p) = &opts.emit_c {
        std::fs::write(p, codegen::standalone(&c_src)).map_err(|e| BuildError::Message(format!("can't write {}: {e}", p.display())))?;
    }
    let (cc, sysroot) = c_compiler();
    // BARM_CFLAGS adds C compiler flags (e.g. "-fsanitize=address,undefined -g" to audit memory safety).
    let extra = std::env::var("BARM_CFLAGS").unwrap_or_default();
    // No FMA contraction: `a * b + c` rounds twice, exactly as in JavaScript (and it's faster on
    // latency-bound loops, where a fused multiply-add lengthens the dependency chain).
    let mut flags: Vec<&str> = vec![opts.opt.as_str(), "-std=gnu11", "-w", "-ffp-contract=off"];
    // Barm code is bounds-checked, so C's stack canaries and fortified memcpy only add checks
    // (and imports: each costs about 50 bytes of binary).
    flags.extend(["-fno-stack-protector", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0"]);
    if opts.unchecked {
        flags.push("-DBARM_UNCHECKED=1");
    }
    if opts.symbols {
        flags.push("-g");
    }
    // The program links with a big main stack (below), so main needn't check for one.
    if cfg!(target_vendor = "apple") && !extra.contains("-fsanitize") {
        flags.push("-DBMG_MAIN_STACK=1");
    }
    if let Some(sdk) = &sysroot {
        flags.push("-isysroot");
        flags.push(sdk);
    }
    flags.extend(extra.split_whitespace());
    let flag_text = flags.join(" ");
    let rt_key = hash_hex(&[codegen::RUNTIME_H.as_bytes(), codegen::RUNTIME_C.as_bytes(), cc.as_bytes(), flag_text.as_bytes(), env!("CARGO_PKG_VERSION").as_bytes()]);
    // A program that fetches links TLS (its code calls bm_tls_install).
    let tls = uses_tls.then(|| TlsArchive::new(&cc, sysroot.as_deref(), &extra));
    // Link flags (see below) are part of what a cached binary was built with.
    let tls_key = tls.as_ref().map(|t| t.key.clone()).unwrap_or_default();
    let key = hash_hex(&[c_src.as_bytes(), rt_key.as_bytes(), LINK_FLAGS.as_bytes(), tls_key.as_bytes()]);
    let dir = cache_dir();
    let bin_dir = dir.join("bin");
    let binary = bin_dir.join(&key);
    if binary.is_file() {
        return Ok(Built { binary, tls: uses_tls, cached: true, sources, timings: (t1 - t0, t2 - t1, Duration::ZERO) });
    }
    let c_dir = dir.join("c");
    for d in [&bin_dir, &c_dir] {
        std::fs::create_dir_all(d).map_err(|e| BuildError::Message(format!("can't create {}: {e}", d.display())))?;
    }
    // The runtime is compiled once per compiler and flags; programs link against the object.
    let rt_obj = c_dir.join(format!("rt-{rt_key}.o"));
    if !rt_obj.is_file() {
        let rt_c = c_dir.join(format!("rt-{rt_key}.c"));
        std::fs::write(&rt_c, codegen::runtime_source()).map_err(|e| BuildError::Message(format!("can't write {}: {e}", rt_c.display())))?;
        let tmp = c_dir.join(format!("rt-{rt_key}.tmp{}.o", std::process::id()));
        let mut cmd = Command::new(&cc);
        cmd.args(&flags).args(["-ffunction-sections", "-fdata-sections", "-c", "-o"]).arg(&tmp).arg(&rt_c);
        run_cc(cmd, &cc, &rt_c)?;
        std::fs::rename(&tmp, &rt_obj).map_err(|e| BuildError::Message(format!("can't move the runtime into the cache: {e}")))?;
    }
    // Every program starts with the same runtime interface and prelude: precompile it once (per
    // compiler and flags) and compile only the program's own code against it.
    let prefix = codegen::fixed_prefix();
    let pch = match c_src.strip_prefix(prefix.as_str()) {
        Some(_) => precompiled_header(&cc, &flags, &c_dir, &prefix, &rt_key),
        None => None,
    };
    let c_path = c_dir.join(format!("{key}.c"));
    let c_text: &str = match &pch {
        Some(_) => &c_src[prefix.len()..],
        None => &c_src,
    };
    std::fs::write(&c_path, c_text).map_err(|e| BuildError::Message(format!("can't write {}: {e}", c_path.display())))?;
    let mut flags = flags.clone();
    let include;
    if let Some(h) = &pch {
        include = h.to_string_lossy().into_owned();
        flags.push("-include");
        flags.push(&include);
    }
    // Compile to a temporary name, then rename: concurrent builds never see a partial binary.
    let tmp = bin_dir.join(format!("{key}.tmp{}", std::process::id()));
    let ld = linker(&cc);
    let mut cmd = Command::new(&ld);
    let obj = c_dir.join(format!("{key}.tmp{}.o", std::process::id()));
    if ld == cc {
        cmd.args(&flags).arg("-o").arg(&tmp).arg(&c_path);
    } else {
        let mut compile = Command::new(&cc);
        compile.args(&flags).arg("-c").arg("-o").arg(&obj).arg(&c_path);
        run_cc(compile, &cc, &c_path)?;
        cmd.args(extra.split_whitespace()).arg("-o").arg(&tmp).arg(&obj);
    }
    cmd.arg(&rt_obj);
    if let Some(t) = &tls {
        cmd.arg(t.build(&cc, &c_dir)?);
        // BoringSSL is C++ (no exceptions or RTTI): a few libc++/libstdc++ helpers
        cmd.arg(if cfg!(target_vendor = "apple") { "-lc++" } else { "-lstdc++" });
    }
    cmd.arg("-lm");
    if !cfg!(target_vendor = "apple") && !cfg!(windows) {
        cmd.arg("-lpthread");
    }
    // Drop the runtime functions the program doesn't use. On macOS the program runs on the main
    // thread with a 512 MiB stack (the most arm64 allows) instead of a thread of its own.
    if cfg!(target_vendor = "apple") {
        // Only `main` is exported: the runtime's functions needn't be in the export table.
        cmd.arg("-Wl,-dead_strip").arg("-Wl,-exported_symbol,_main");
        if !opts.symbols {
            cmd.arg("-Wl,-x"); // local symbols (function names) only serve debuggers and profilers
        }
        // (Sanitizers can't lay out their shadow memory around a large main stack.)
        if !extra.contains("-fsanitize") {
            cmd.arg(LINK_STACK);
        }
    } else if !cfg!(windows) {
        cmd.arg("-Wl,--gc-sections");
        if !opts.symbols {
            cmd.arg("-Wl,-x");
        }
    }
    let linked = run_cc(cmd, &ld, &c_path);
    let _ = std::fs::remove_file(&obj);
    linked?;
    std::fs::rename(&tmp, &binary).map_err(|e| BuildError::Message(format!("can't move the binary into the cache: {e}")))?;
    let t3 = Instant::now();
    Ok(Built { binary, tls: uses_tls, cached: false, sources, timings: (t1 - t0, t2 - t1, t3 - t2) })
}

/// macOS: the main thread's stack size (the most arm64 allows).
const LINK_STACK: &str = "-Wl,-stack_size,0x20000000";
/// Everything that changes how programs are linked, for the binary cache key.
const LINK_FLAGS: &str = "dead-strip; exported: _main; -x unless -g; sanitizers link without -stack_size; -Wl,-stack_size,0x20000000";

/// The precompiled fixed prefix (`pre-<key>.h`, with its `.pch`/`.gch` beside it) to pass as
/// `-include`, built on first use; `None` if the compiler can't precompile it (then programs
/// are compiled whole, which is only slower).
fn precompiled_header(cc: &str, flags: &[&str], c_dir: &Path, prefix: &str, rt_key: &str) -> Option<PathBuf> {
    let clang = cfg!(target_vendor = "apple") || Path::new(cc).file_name().is_some_and(|n| n.to_string_lossy().contains("clang"));
    let key = hash_hex(&[prefix.as_bytes(), rt_key.as_bytes()]);
    let header = c_dir.join(format!("pre-{key}.h"));
    let pch = c_dir.join(format!("pre-{key}.h.{}", if clang { "pch" } else { "gch" }));
    if pch.is_file() && header.is_file() {
        return Some(header);
    }
    let pid = std::process::id();
    let tmp_h = c_dir.join(format!("pre-{key}.tmp{pid}.h"));
    let tmp_p = c_dir.join(format!("pre-{key}.tmp{pid}.pch"));
    // The header goes to its final path first (the .pch records it; its content is fixed by the
    // key, so a concurrent build writes the same bytes), then the .pch appears atomically.
    if !header.is_file() {
        std::fs::write(&tmp_h, prefix).ok()?;
        std::fs::rename(&tmp_h, &header).ok()?;
    }
    let ok = Command::new(cc).args(flags).args(["-x", "c-header", "-o"]).arg(&tmp_p).arg(&header).output().is_ok_and(|o| o.status.success());
    if !ok {
        let _ = std::fs::remove_file(&tmp_p);
        return None;
    }
    std::fs::rename(&tmp_p, &pch).ok()?;
    Some(header)
}

fn run_cc(mut cmd: Command, cc: &str, c_path: &Path) -> Result<(), BuildError> {
    let out = cmd.output().map_err(|e| BuildError::Message(format!("can't run the C compiler `{cc}`: {e} (set CC to choose one)")))?;
    if !out.status.success() {
        let stderr = String::from_utf8_lossy(&out.stderr);
        let first: String = stderr.lines().take(30).collect::<Vec<_>>().join("\n");
        return Err(BuildError::Message(format!(
            "internal error: the generated C failed to compile (this is a bug in barm)\n  C file: {}\n{first}",
            c_path.display()
        )));
    }
    Ok(())
}

/// The TLS archive: runtime/tls.c and BoringSSL (vendor/boringssl), compiled once per compiler,
/// SDK and sanitizer flags. Its flags are fixed (-O2 whatever the program's -O, and no
/// --unchecked), so other builds share it; only fetch() programs link it.
struct TlsArchive {
    key: String,
    /// flags for C, assembly and C++ alike
    flags: Vec<String>,
}

impl TlsArchive {
    fn new(cc: &str, sysroot: Option<&str>, extra: &str) -> TlsArchive {
        let mut flags: Vec<String> = ["-O3", "-DNDEBUG", "-w", "-fno-stack-protector", "-U_FORTIFY_SOURCE", "-D_FORTIFY_SOURCE=0", "-ffunction-sections", "-fdata-sections", "-fno-strict-aliasing", "-fvisibility=hidden"]
            .map(String::from)
            .to_vec();
        if let Some(sdk) = sysroot {
            flags.push("-isysroot".into());
            flags.push(sdk.into());
        }
        if extra.contains("-fsanitize") {
            flags.extend(extra.split_whitespace().map(String::from));
        }
        let key = hash_hex(&[tls_files::TLS_KEY.as_bytes(), codegen::RUNTIME_H.as_bytes(), cc.as_bytes(), flags.join(" ").as_bytes()]);
        TlsArchive { key, flags }
    }

    /// The C++ compiler that goes with `cc` (CXX overrides it).
    fn cxx(cc: &str) -> String {
        if let Ok(cxx) = std::env::var("CXX") {
            return cxx;
        }
        let path = Path::new(cc);
        let name = path.file_name().map(|n| n.to_string_lossy().into_owned()).unwrap_or_default();
        let sibling = |n: String| path.with_file_name(n).to_string_lossy().into_owned();
        if let Some(rest) = name.strip_prefix("clang") {
            return sibling(format!("clang++{rest}"));
        }
        if let Some(rest) = name.strip_prefix("gcc") {
            return sibling(format!("g++{rest}"));
        }
        if name == "cc" {
            return sibling("c++".into());
        }
        "c++".into()
    }

    /// The archive, compiling it first if it isn't cached.
    fn build(&self, cc: &str, c_dir: &Path) -> Result<PathBuf, BuildError> {
        let archive = c_dir.join(format!("tls-{}.a", self.key));
        if archive.is_file() {
            return Ok(archive);
        }
        eprintln!("barm: compiling the TLS library (BoringSSL, brotli, zlib, zstd); this happens once");
        let fail = |what: String| BuildError::Message(format!("can't build the TLS library: {what}"));
        let dir = c_dir.join(format!("tls-{}.tmp{}", self.key, std::process::id()));
        for (rel, bytes) in tls_files::TLS_FILES.iter().copied().chain([("runtime/barm.h", codegen::RUNTIME_H.as_bytes())]) {
            let path = dir.join(rel);
            std::fs::create_dir_all(path.parent().unwrap()).map_err(|e| fail(format!("{e}")))?;
            std::fs::write(&path, bytes).map_err(|e| fail(format!("{e}")))?;
        }
        let sources: Vec<&str> = tls_files::TLS_FILES.iter().map(|f| f.0).filter(|p| p.ends_with(".cc") || p.ends_with(".S") || p.ends_with(".c")).collect();
        let cxx = Self::cxx(cc);
        let include = ["vendor/boringssl/include", "vendor/brotli/c/include", "vendor/zlib", "vendor/zstd/lib", "runtime"].map(|p| format!("-I{}", dir.join(p).display()));
        // compile in parallel: ~400 files, once
        let threads = std::thread::available_parallelism().map_or(4, |n| n.get()).min(sources.len());
        let next = std::sync::atomic::AtomicUsize::new(0);
        let errors = std::sync::Mutex::new(Vec::new());
        std::thread::scope(|s| {
            for _ in 0..threads {
                s.spawn(|| loop {
                    let i = next.fetch_add(1, std::sync::atomic::Ordering::Relaxed);
                    let Some(src) = sources.get(i) else { break };
                    let (compiler, lang): (&str, &[&str]) = if src.ends_with(".cc") {
                        (&cxx, &["-std=c++17", "-fno-exceptions", "-fno-rtti"])
                    } else if src.ends_with(".c") {
                        (cc, &["-std=gnu11"])
                    } else {
                        (cc, &[])
                    };
                    let out = Command::new(compiler).args(&self.flags).args(lang).args(&include).arg("-c").arg(dir.join(src)).arg("-o").arg(dir.join(format!("{i}.o"))).output();
                    match out {
                        Ok(o) if o.status.success() => {}
                        Ok(o) => errors.lock().unwrap().push(format!("{src}: {}", String::from_utf8_lossy(&o.stderr).lines().take(5).collect::<Vec<_>>().join("\n"))),
                        Err(e) => errors.lock().unwrap().push(format!("{src}: can't run `{compiler}`: {e}")),
                    }
                });
            }
        });
        if let Some(e) = errors.into_inner().unwrap().first() {
            let _ = std::fs::remove_dir_all(&dir);
            return Err(fail(e.clone()));
        }
        // the toolchain's own archiver (next to the compiler we picked), else the one on PATH
        let ar = Path::new(cc).parent().map(|d| d.join("ar")).filter(|p| p.is_file()).map_or("ar".to_string(), |p| p.to_string_lossy().into_owned());
        let tmp = c_dir.join(format!("tls-{}.tmp{}.a", self.key, std::process::id()));
        let objs: Vec<PathBuf> = (0..sources.len()).map(|i| dir.join(format!("{i}.o"))).collect();
        let out = Command::new(&ar).arg("rcs").arg(&tmp).args(&objs).output().map_err(|e| fail(format!("can't run `{ar}`: {e}")))?;
        let _ = std::fs::remove_dir_all(&dir);
        if !out.status.success() {
            let _ = std::fs::remove_file(&tmp);
            return Err(fail(format!("`{ar}` failed: {}", String::from_utf8_lossy(&out.stderr))));
        }
        std::fs::rename(&tmp, &archive).map_err(|e| {
            let _ = std::fs::remove_file(&tmp);
            fail(format!("can't move it into the cache: {e}"))
        })?;
        Ok(archive)
    }
}

/// A 128-bit content hash (two independent 64-bit lanes), hex-encoded. For cache keys, not security.
fn hash_hex(parts: &[&[u8]]) -> String {
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
