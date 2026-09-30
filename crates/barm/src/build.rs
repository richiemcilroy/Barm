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

pub enum BuildError {
    Diagnostics(SourceMap, Vec<Diagnostic>),
    Message(String),
}

pub struct Built {
    pub binary: PathBuf,
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
/// than the system compiler, and generates faster code), else `cc`.
fn c_compiler() -> String {
    if let Ok(cc) = std::env::var("CC") {
        return cc;
    }
    for candidate in ["/opt/homebrew/opt/llvm/bin/clang", "/usr/local/opt/llvm/bin/clang", "/usr/lib/llvm-21/bin/clang", "/usr/lib/llvm-20/bin/clang"] {
        if Path::new(candidate).is_file() {
            return candidate.to_string();
        }
    }
    "cc".into()
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
    let c_src = match codegen::generate(&mut checker, opts.mode) {
        Ok(c) => c,
        Err(d) => {
            drop(checker);
            return Err(BuildError::Diagnostics(sm, d));
        }
    };
    drop(checker);
    let t2 = Instant::now();
    if let Some(p) = &opts.emit_c {
        std::fs::write(p, codegen::standalone(&c_src)).map_err(|e| BuildError::Message(format!("can't write {}: {e}", p.display())))?;
    }
    let cc = c_compiler();
    // BARM_CFLAGS adds C compiler flags (e.g. "-fsanitize=address,undefined -g" to audit memory safety).
    let extra = std::env::var("BARM_CFLAGS").unwrap_or_default();
    // No FMA contraction: `a * b + c` rounds twice, exactly as in JavaScript (and it's faster on
    // latency-bound loops, where a fused multiply-add lengthens the dependency chain).
    let mut flags: Vec<&str> = vec![opts.opt.as_str(), "-std=gnu11", "-w", "-ffp-contract=off"];
    if opts.unchecked {
        flags.push("-DBARM_UNCHECKED=1");
    }
    flags.extend(extra.split_whitespace());
    let flag_text = flags.join(" ");
    let rt_key = hash_hex(&[codegen::RUNTIME_H.as_bytes(), codegen::RUNTIME_C.as_bytes(), cc.as_bytes(), flag_text.as_bytes(), env!("CARGO_PKG_VERSION").as_bytes()]);
    let key = hash_hex(&[c_src.as_bytes(), rt_key.as_bytes()]);
    let dir = cache_dir();
    let bin_dir = dir.join("bin");
    let binary = bin_dir.join(&key);
    if binary.is_file() {
        return Ok(Built { binary, cached: true, sources, timings: (t1 - t0, t2 - t1, Duration::ZERO) });
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
    let c_path = c_dir.join(format!("{key}.c"));
    std::fs::write(&c_path, &c_src).map_err(|e| BuildError::Message(format!("can't write {}: {e}", c_path.display())))?;
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
    cmd.arg(&rt_obj).arg("-lm");
    if !cfg!(target_vendor = "apple") && !cfg!(windows) {
        cmd.arg("-lpthread");
    }
    // Drop the runtime functions the program doesn't use.
    if cfg!(target_vendor = "apple") {
        cmd.arg("-Wl,-dead_strip");
    } else if !cfg!(windows) {
        cmd.arg("-Wl,--gc-sections");
    }
    let linked = run_cc(cmd, &ld, &c_path);
    let _ = std::fs::remove_file(&obj);
    linked?;
    std::fs::rename(&tmp, &binary).map_err(|e| BuildError::Message(format!("can't move the binary into the cache: {e}")))?;
    let t3 = Instant::now();
    Ok(Built { binary, cached: false, sources, timings: (t1 - t0, t2 - t1, t3 - t2) })
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
